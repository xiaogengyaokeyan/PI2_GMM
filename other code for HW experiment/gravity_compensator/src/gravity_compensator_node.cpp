#include "gravity_compensator_node.h"
#include <iostream>
#include <fstream>

GravityCompensator::GravityCompensator(int _sampleSize):
    sampleSize(_sampleSize),
    estimatedParameters(10, true),
    recordedRotAxis(3,_sampleSize, true),
    recordedRotAngle(_sampleSize, true),
    recordedWrench(6,_sampleSize, true),
  //  recordedR3(3,_sampleSize,true),
    recCountPos(0),
    recCountWrench(0),
    modelReady(false),
    compensate(false),
    poseReady(false),
    wrenchReady(false),
    recLimit(0),
    g(0.0,0.0,-9.82),
    toolR3(0, 1, 0, 0, 0, 1, 1, 0, 0),
    recordedObsMatrix(6*_sampleSize, 10, true),
    newObsMatrix(6, 10, true),
    hf(3,10),
    ht(3,10),
    wrenchVect(_sampleSize*6),
    compensatedWrenches(6,true),
    compensatedWrenchesBase(6,true),
    newWrenchData(6,true)
{
     pubBase = nodeHandle.advertise<geometry_msgs::WrenchStamped>("comp_wrenchBase_pub", 1000);
     pubEE =  nodeHandle.advertise<geometry_msgs::WrenchStamped>("comp_wrenchEE_pub", 1000);
     pubPose = nodeHandle.advertise<geometry_msgs::PoseStamped>("comp_pose_pub",1000);
    // poseSub = nodeHandle.subscribe("WAM/Pose", 1000, &GravityCompensator::posSubCallback, this);
     poseSub = nodeHandle.subscribe(TOPIC_NAME_POSE, 1000, &GravityCompensator::posSubCallback, this);
    // wrenchSub = nodeHandle.subscribe("WAM/sensed_wrench_ee", 1000, &GravityCompensator::wrenchSubCallback, this);
     wrenchSub = nodeHandle.subscribe("netft_data", 1000, &GravityCompensator::wrenchSubCallback, this);
     ctrlSub = nodeHandle.subscribe("GravityCompensator/ctrl", 1000, &GravityCompensator::ctrlSubCallback, this);
}

void GravityCompensator::ctrlSubCallback(const std_msgs::Int32 command)
{
    switch(command.data){
    case 8: ROS_INFO("taring the offset"); tareOffset(); break;
    case 7: ROS_INFO("Model loaded from model.txt"); loadModel(); modelReady = true; break;
    case 6: ROS_INFO("Model saved to file model.txt"); saveModel(); break;
    case 5: ROS_INFO("Estimated parameters: "); estimatedParameters.Print(); break;
    case 4: compensate = !compensate; ROS_INFO("Compensation on = %d ",compensate); break;
    case 3: if(recLimit<sampleSize)recLimit = recLimit +4; if(recLimit>sampleSize) recLimit == sampleSize; ROS_INFO("Recording a few sample, tot recorded = %d", recLimit); break;
    case 2: recLimit = sampleSize;  ROS_INFO("Start recording full sample"); break;
    case 1: if(recLimit==0)recLimit = sampleSize/2; else recLimit = sampleSize; ROS_INFO("Start recording half of tot sample"); break;
    case 0: poseReady=false; wrenchReady=false; modelReady=false; recCountPos=0; recCountWrench=0; recLimit=0; ROS_INFO("Reset"); break;
    }
}

void GravityCompensator::tareOffset(){
    if(!modelReady){
        return;
    }
    for(int i=0;i<3;i++){
        estimatedParameters(1+i) += compensatedWrenches(i);
        estimatedParameters(7+i) += compensatedWrenches(3+i);
    }
}

void GravityCompensator::posSubCallback(const geometry_msgs::PoseStamped poseMsg)
{
  //  ROS_INFO("Begin poseSubCallback");
    // compute new rot angle axis from quaternions

    latestPoseMsg = poseMsg;

    newRotAngle = 2*acos(poseMsg.pose.orientation.w); // angle
    if(newRotAngle !=0.0)
    {
        newRotAxis(0) = poseMsg.pose.orientation.x/sin(newRotAngle /2); // axis
        newRotAxis(1) = poseMsg.pose.orientation.y/sin(newRotAngle /2);
        newRotAxis(2) = poseMsg.pose.orientation.z/sin(newRotAngle /2);
    }
    else
    {
        newRotAxis(0) = 0;
        newRotAxis(1) = 0;
        newRotAxis(2) = 0;
    }

    newR3(0,0) = poseMsg.pose.orientation.w*poseMsg.pose.orientation.w;
    newR3(0,0) += poseMsg.pose.orientation.x*poseMsg.pose.orientation.x;
    newR3(0,0) -= poseMsg.pose.orientation.y*poseMsg.pose.orientation.y;
    newR3(0,0) -= poseMsg.pose.orientation.z*poseMsg.pose.orientation.z;

    newR3(0,1) = 2*poseMsg.pose.orientation.x*poseMsg.pose.orientation.y-2*poseMsg.pose.orientation.z*poseMsg.pose.orientation.w;
    newR3(0,2) = 2*poseMsg.pose.orientation.x*poseMsg.pose.orientation.z+2*poseMsg.pose.orientation.w*poseMsg.pose.orientation.y;

    newR3(1,0) = 2*poseMsg.pose.orientation.x*poseMsg.pose.orientation.y+2*poseMsg.pose.orientation.z*poseMsg.pose.orientation.w;

    newR3(1,1) = poseMsg.pose.orientation.w*poseMsg.pose.orientation.w;
    newR3(1,1) -= poseMsg.pose.orientation.x*poseMsg.pose.orientation.x;
    newR3(1,1) += poseMsg.pose.orientation.y*poseMsg.pose.orientation.y;
    newR3(1,1) -= poseMsg.pose.orientation.z*poseMsg.pose.orientation.z;

    newR3(1,2) = 2*poseMsg.pose.orientation.y*poseMsg.pose.orientation.z-2*poseMsg.pose.orientation.x*poseMsg.pose.orientation.w;

    newR3(2,0) = 2*poseMsg.pose.orientation.x*poseMsg.pose.orientation.z-2*poseMsg.pose.orientation.w*poseMsg.pose.orientation.y;
    newR3(2,1) = 2*poseMsg.pose.orientation.y*poseMsg.pose.orientation.z+2*poseMsg.pose.orientation.w*poseMsg.pose.orientation.x;

    newR3(2,2) = poseMsg.pose.orientation.w*poseMsg.pose.orientation.w;
    newR3(2,2) -= poseMsg.pose.orientation.x*poseMsg.pose.orientation.x;
    newR3(2,2) -= poseMsg.pose.orientation.y*poseMsg.pose.orientation.y;
    newR3(2,2) += poseMsg.pose.orientation.z*poseMsg.pose.orientation.z;

    poseReady = true;

    if(recCountPos<recLimit)
    {
        recordedRotAngle(recCountPos) = newRotAngle;
        recordedRotAxis.SetColumn(newRotAxis,recCountPos);
      //  recordedR3.SetColumn(newR3,recCountPos);
        computeObservationMatrix(true);
        recCountPos++;
    //    ROS_INFO("I got a pose message, recCountPos = %d", recCountPos);
    }
    if(recCountPos==sampleSize && recCountWrench == sampleSize && !modelReady){
        computeEstimatedParams();
    }
    if(modelReady && poseReady && wrenchReady && compensate){
        computeCompensatedWrenches();
    }

}

void GravityCompensator::wrenchSubCallback(const geometry_msgs::WrenchStamped wrenchMsg)
{
  //  ROS_INFO("Begin wrenchSubCallback");
    newWrenchData(0) = wrenchMsg.wrench.force.x;
    newWrenchData(1) = wrenchMsg.wrench.force.y; // the ref frame of the nano25 f/t sensor is left handed
    newWrenchData(2) = wrenchMsg.wrench.force.z;
    newWrenchData(3) = wrenchMsg.wrench.torque.x;
    newWrenchData(4) = wrenchMsg.wrench.torque.y;
    newWrenchData(5) = wrenchMsg.wrench.torque.z;
    wrenchReady = true;
    latestWrenchMsg = wrenchMsg;
    if(recCountWrench<recLimit)
    {
        recordedWrench.SetColumn(newWrenchData,recCountWrench);
        wrenchVect.SetSubVector(recCountWrench*6, newWrenchData);
        recCountWrench ++;
      //  ROS_INFO("I got a wrench message, recCountWrench = %d", recCountWrench);
    }
    if(recCountPos==sampleSize && recCountWrench == sampleSize && !modelReady){
        computeEstimatedParams();
    }
    if(modelReady && poseReady && wrenchReady && compensate){
        computeCompensatedWrenches();
    }

}

void GravityCompensator::computeObservationMatrix(bool record)
{
  //  ROS_INFO("Begin computeObservationMatrix");
    //rotMat.RotationV(newRotAngle,newRotAxis);
   // phi = rotMat*g;

    //newR3.TransposeMult(g,phi);
   // phi = R*g


    // ROS_INFO("PHI");
    //toolR3.Mult(phi,phi);
    //phi = toolR3*phi;

    Matrix3 newRt;
    //newRt.Print();
    newRt = newR3.Transpose();
    Vector3 g_wrist,g_sensor;
    g_wrist = newRt*g;
    //cout<<"wrist: "<<endl;
    //g_wrist.Print();
    //g_sensor =toolR3.Transpose()*g_wrist;
    //Matrix3 toolR3T = toolR3.Transpose();
    //toolR3.Print();
    g_sensor = toolR3*g_wrist;
    //cout<<"sensor: "<<endl;
    //g_sensor.Print();
    phi = g_sensor;

    //phi.Print();
    hf.SetColumn(phi,0);
    hf.SetColumnSpace(Matrix3::IDENTITY,1);
    hf.SetColumnSpace(Matrix3::ZERO,4);
    hf.SetColumnSpace(Matrix3::ZERO,7);
    crossMat.Set(0, phi(2), -phi(1), -phi(2), 0, phi(0), phi(1), -phi(0), 0);
    ht.SetColumn(Vector3::ZERO,1);
    ht.SetColumnSpace(Matrix3::ZERO,2);
    ht.SetColumnSpace(crossMat,4);
    ht.SetColumnSpace(Matrix3::IDENTITY,7);
    if(record)
    {
        newObsMatrix.SetRowSpace(hf,0);
        newObsMatrix.SetRowSpace(ht,3);

        recordedObsMatrix.SetRowSpace(newObsMatrix,recCountPos*6);
    }
    else
    {
        newObsMatrix.SetRowSpace(hf,0);
        newObsMatrix.SetRowSpace(ht,3);
    }
   // newObsMatrix.Print();
}

void GravityCompensator::computeEstimatedParams(void)
{
  //  ROS_INFO("computeEstimatedParams");
    estimatedParameters.Set(recordedObsMatrix.Inverse()*wrenchVect);
   // recordedObsMatrix.Print();
   // recordedObsMatrix.Inverse().Print();
    modelReady = true;
    estimatedParameters.Print();
//    ROS_INFO("Model computation, estimated param: %f %f %f %f %f %f %f %f %f %f", estimatedParameters(0), estimatedParameters(1), estimatedParameters(2), estimatedParameters(3), estimatedParameters(4), estimatedParameters(5), estimatedParameters(6), estimatedParameters(7), estimatedParameters(8), estimatedParameters(9));
    ROS_INFO("Recoreded Wrench:");
    recordedWrench.Print();
    ROS_INFO("Recoreded Axis:");
    recordedRotAxis.Print();
    ROS_INFO("Recoreded Angles:");
    recordedRotAngle.Print();
}

void GravityCompensator::computeCompensatedWrenches()
{
  //  ROS_INFO("Begin computeCompensatedWrenches");
    computeObservationMatrix(false);
    compensatedWrenches = newWrenchData - newObsMatrix*estimatedParameters;

    compensatedForces(0) = compensatedWrenches(0);
    compensatedForces(1) = compensatedWrenches(1);
    compensatedForces(2) = compensatedWrenches(2);
    pubMessageEE.wrench.force.x = compensatedWrenches(0);
    pubMessageEE.wrench.force.y = compensatedWrenches(1);
    pubMessageEE.wrench.force.z = compensatedWrenches(2);
    compensatedTorques(0) = compensatedWrenches(3);
    compensatedTorques(1) = compensatedWrenches(4);
    compensatedTorques(2) = compensatedWrenches(5);
    pubMessageEE.wrench.torque.x = compensatedWrenches(3);
    pubMessageEE.wrench.torque.y = compensatedWrenches(4);
    pubMessageEE.wrench.torque.z = compensatedWrenches(5);

    pubMessageEE.header.stamp = latestWrenchMsg.header.stamp;
    pubEE.publish(pubMessageEE);
   // ROS_INFO("compensated Wrenches EE: ");
   // compensatedWrenches.Print();

    Vector3 tempForces;
    toolR3.TransposeMult(compensatedForces, tempForces);
    newR3.Mult(tempForces, compensatedForcesBase);

    Vector3 tempTorques;
    toolR3.TransposeMult(compensatedTorques, tempTorques);
    newR3.Mult(tempTorques,compensatedTorquesBase);

    compensatedWrenchesBase(0) = compensatedForcesBase(0);
    compensatedWrenchesBase(1) = compensatedForcesBase(1);
    compensatedWrenchesBase(2) = compensatedForcesBase(2);
    compensatedWrenchesBase(3) = compensatedTorquesBase(0);
    compensatedWrenchesBase(4) = compensatedTorquesBase(1);
    compensatedWrenchesBase(5) = compensatedTorquesBase(2);

    pubMessageBase.wrench.force.x = compensatedWrenchesBase(0);
    pubMessageBase.wrench.force.y = compensatedWrenchesBase(1);
    pubMessageBase.wrench.force.z = compensatedWrenchesBase(2);
    pubMessageBase.wrench.torque.x = compensatedWrenchesBase(3);
    pubMessageBase.wrench.torque.y = compensatedWrenchesBase(4);
    pubMessageBase.wrench.torque.z = compensatedWrenchesBase(5);

    pubMessageBase.header.stamp = latestWrenchMsg.header.stamp;
    pubBase.publish(pubMessageBase);
   // ROS_INFO("compensated Wrenches Base: ");
  //  compensatedWrenchesBase.Print();

  //  ROS_INFO("raw Wrenches: ");
  //  newWrenchData.Print();
    wrenchReady = false;
    poseReady = false;

    pubPose.publish(latestPoseMsg);  // the pose is published at the same frequency as the compensated wrench in order to synchronize the sources
}

int GravityCompensator::saveModel()
{
  ofstream myfile;
  myfile.open ("model.txt");
  myfile << estimatedParameters;
  myfile.close();
  return 0;
}

int GravityCompensator::loadModel()
{
  ifstream myfile;
  string str;
  myfile.open ("/home/joel/catkin_ws/src/gravity_compensator/model.txt");
  getline(myfile, str);
  std::vector<float> v;
  std::istringstream iss(str);
  std::copy(std::istream_iterator<float>(iss),
    std::istream_iterator<float>(),
    std::back_inserter(v));

  for(int i=0;i<v.size(); i++)
  {
      estimatedParameters(i) = v[i];
  }

  cout << v[0] <<   v[1] << "\n";
  estimatedParameters.Print();
  myfile.close();
  return 0;
}



int main(int argc, char **argv)
{
    ros::init(argc, argv, "CompensateGravity");

    GravityCompensator compensator(20);

    ros::spin();
    return 0;
}
