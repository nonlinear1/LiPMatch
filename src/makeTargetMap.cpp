#include <makeTargetmapcore.h>
#include <math.h>
#include <vector>
#include <aloam_velodyne/common.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseStamped.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/search/search.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <tf/tf.h> 
#include <eigen3/Eigen/Dense>
#include <ceres/ceres.h>
#include <mutex>
#include <queue>
#include <thread>
#include <iostream>
#include <string>
#include <visualization_msgs/Marker.h>
#include <visualization_msgs/MarkerArray.h>

#include "lidarFactor.hpp"
#include "aloam_velodyne/common.h"
#include "aloam_velodyne/tic_toc.h"
#include "RangenetAPI.hpp"

#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>

#include <tools.h>

using namespace std;
using namespace Makemap_ns;


int frameCount = 0;

double timeLaserOdometry = 0;

double parameters[7] = {0, 0, 0, 1, 0, 0, 0};
Eigen::Map<Eigen::Quaterniond> q_w_curr(parameters);
Eigen::Map<Eigen::Vector3d> t_w_curr(parameters + 4);

Eigen::Vector3d t_w_prev(0.0,0.0,0.0);

pcl::VoxelGrid<PointType> downSizeFilterKF;


ros::Publisher pubLaserCloudFullRes, pubOdomAftMapped, pubLaserAfterMappedPath;

ros::Publisher pubMatchedPoints1;

ros::Publisher pubMatchedPoints2;

ros::Publisher pubEachFrameLaserCloud;

nav_msgs::Path laserAfterMappedPath;

Makemap lipmatch;

std::list<tools::m_keyframe> m_keyframe_of_updating_list;
std::list<tools::m_keyframe> m_keyframe_need_precession_list;

std::vector<float> read_lidar_data(const std::string lidar_data_path)
{
    std::ifstream lidar_data_file(lidar_data_path, std::ifstream::in | std::ifstream::binary);
    lidar_data_file.seekg(0, std::ios::end);
    const size_t num_elements = lidar_data_file.tellg() / sizeof(float);
    lidar_data_file.seekg(0, std::ios::beg);

    std::vector<float> lidar_data_buffer(num_elements);
    lidar_data_file.read(reinterpret_cast<char*>(&lidar_data_buffer[0]), num_elements*sizeof(float));
    return lidar_data_buffer;
}



void pointAssociateToMap(PointType const *const pi, PointType *const po)
{
    Eigen::Vector3d point_curr(pi->x, pi->y, pi->z);
    Eigen::Vector3d point_w = q_w_curr * point_curr + t_w_curr;
    po->x = point_w.x();
    po->y = point_w.y();
    po->z = point_w.z();
    po->intensity = pi->intensity;
    //po->intensity = 1.0;
}


void process()
{
    // std::ifstream timestamp_file("/home/jjwen/data/KITTI/odometry/05.txt", std::ifstream::in);

    // std::ifstream timestamp_file("/media/jjwen/SBPD1_ddy/bk ubuntu18 6月7号/data/KITTI/odometry/05.txt", std::ifstream::in);
    std::ifstream timestamp_file("/media/jjwen/SBPD1_ddy/bk ubuntu18 6月7号/data/KITTI/odometry/00.txt", std::ifstream::in);


    std::string line;
    std::size_t line_num = 0;

    ros::Rate r(15.0);

//    ros::Rate r(1.0);

    // while (std::getline(timestamp_file, line) && ros::ok())
    while (std::getline(timestamp_file, line) && ros::ok() && line_num < 3300)
    {
        std::vector<double> vdata;
        std::stringstream pose_stream(line);
        std::string s;
        for (std::size_t i = 0; i < 3; ++i)
        {
            for (std::size_t j = 0; j < 4; ++j)
            {
                std::getline(pose_stream, s, ' ');
                vdata.push_back(stof(s));
            }
        }

        Eigen::Matrix3d rotation_matrix;
        rotation_matrix<<vdata[0],vdata[1],vdata[2],vdata[4],vdata[5],vdata[6],vdata[8],vdata[9],vdata[10];
        Eigen::Quaterniond quat(rotation_matrix);

        parameters[0] = quat.z();
        parameters[1] = -quat.x();
        parameters[2] = -quat.y();
        parameters[3] = quat.w();
        parameters[4] = vdata[11];
        parameters[5] = -vdata[3];
        parameters[6] = -vdata[7];

        pcl::PointCloud<pcl::PointXYZI> laserCloudIn;

        // string binpath = "/home/jjwen/data/KITTI/odometry/05/semantic/"+to_string(line_num)+".bin";

        // string binpath = "/media/jjwen/SBPD1_ddy/bk ubuntu18 6月7号/data/KITTI/odometry/05/semantic/"+to_string(line_num)+".bin";
        // line_num++;

        line_num++;
        string binpath = "/media/jjwen/SBPD1_ddy/bk ubuntu18 6月7号/data/KITTI/odometry/00/semantic/"+to_string(line_num)+".bin";




        std::vector<float> lidar_data = read_lidar_data(binpath);
        for (std::size_t i = 0; i < lidar_data.size(); i += 4)
        {
            pcl::PointXYZI point;
            point.x = lidar_data[i];
            point.y = lidar_data[i + 1];
            point.z = lidar_data[i + 2];
            point.intensity = lidar_data[i + 3];
            laserCloudIn.push_back(point);
        }

        uint32_t num_points = laserCloudIn.points.size();

        pcl::PointCloud<pcl::PointXYZI>::Ptr structure_points(new pcl::PointCloud<pcl::PointXYZI>());
        pcl::PointCloud<pcl::PointXYZI>::Ptr vehicle_points(new pcl::PointCloud<pcl::PointXYZI>());
        pcl::PointCloud<pcl::PointXYZI>::Ptr nature_points(new pcl::PointCloud<pcl::PointXYZI>());

        for (uint32_t i = 0; i < num_points; ++i) {

            int label = int(laserCloudIn.points[i].intensity);

            //structure
            if (label == 50 || label == 51 || label == 52 || label == 60) {
                structure_points->points.push_back(laserCloudIn.points[i]);
            }
                //vehicle
            else if (label == 10 || label == 13 || label == 18 || label == 16) {
                vehicle_points->points.push_back(laserCloudIn.points[i]);
            }
                //cylinder
            else if (label == 71 || label == 80) {
                nature_points->points.push_back(laserCloudIn.points[i]);
            }
        }

        downSizeFilterKF.setInputCloud(structure_points);
        downSizeFilterKF.filter(*structure_points);

        downSizeFilterKF.setInputCloud(vehicle_points);
        downSizeFilterKF.filter(*vehicle_points);

        downSizeFilterKF.setInputCloud(nature_points);
        downSizeFilterKF.filter(*nature_points);

        for (int i = 0; i < structure_points->points.size(); i++)
        {
            pointAssociateToMap(&structure_points->points[i], &structure_points->points[i]);
        }

        for (int i = 0; i < vehicle_points->points.size(); i++)
        {
            pointAssociateToMap(&vehicle_points->points[i], &vehicle_points->points[i]);
        }

        for (int i = 0; i < nature_points->points.size(); i++)
        {
            pointAssociateToMap(&nature_points->points[i], &nature_points->points[i]);
        }

        for(auto it = m_keyframe_of_updating_list.begin(); it != m_keyframe_of_updating_list.end(); it++ )
        {
            *(it->structurelaserCloud) += *structure_points;
            *(it->vehiclelaserCloud) += *vehicle_points;
            *(it->naturelaserCloud) += *nature_points;
            it->framecount++;
            it->travel_length += sqrt((t_w_curr[0]-t_w_prev[0])*(t_w_curr[0]-t_w_prev[0])+
                                      (t_w_curr[1]-t_w_prev[1])*(t_w_curr[1]-t_w_prev[1])+(t_w_curr[2]-t_w_prev[2])*(t_w_curr[2]-t_w_prev[2]));
        }

        t_w_prev = t_w_curr;


        //30 32
        if ( m_keyframe_of_updating_list.front().travel_length >= 30.0 )
        {
            m_keyframe_of_updating_list.front().m_ending_frame_idx = frameCount;
            m_keyframe_need_precession_list.push_back( m_keyframe_of_updating_list.front() );
            m_keyframe_of_updating_list.pop_front();

            lipmatch.frameQueue.push_back(m_keyframe_need_precession_list.back());
        }

        //25 26
        if ( m_keyframe_of_updating_list.back().travel_length >= 25.0 )
        {
            tools::m_keyframe tk1;
            m_keyframe_of_updating_list.push_back(tk1);
        }

        sensor_msgs::PointCloud2 matchedCloudOutMsg1;
        pcl::toROSMsg(lipmatch.laserCloudOri_mp1, matchedCloudOutMsg1);
        matchedCloudOutMsg1.header.stamp = ros::Time().fromSec(timeLaserOdometry);
        matchedCloudOutMsg1.header.frame_id = "/camera_init";
        pubMatchedPoints1.publish(matchedCloudOutMsg1);

//        sensor_msgs::PointCloud2 matchedCloudOutMsg2;
//        pcl::toROSMsg(lipmatch.laserCloudOri_m2_1, matchedCloudOutMsg2);
//        matchedCloudOutMsg2.header.stamp = ros::Time().fromSec(timeLaserOdometry);
//        matchedCloudOutMmarker_keyframe_pubsg2.header.frame_id = "/camera_init";
//        pubMatchedPoints2.publish(matchedCloudOutMsg2);

        sensor_msgs::PointCloud2 matchedCloudOutMsg2;
        pcl::toROSMsg(lipmatch.laserCloudOri_m2, matchedCloudOutMsg2);
        matchedCloudOutMsg2.header.stamp = ros::Time().fromSec(timeLaserOdometry);
        matchedCloudOutMsg2.header.frame_id = "/camera_init";
        pubMatchedPoints2.publish(matchedCloudOutMsg2);

        sensor_msgs::PointCloud2 eachPointCloudOutMsg;
        pcl::toROSMsg(*vehicle_points, eachPointCloudOutMsg);
        eachPointCloudOutMsg.header.stamp = ros::Time().fromSec(timeLaserOdometry);
        eachPointCloudOutMsg.header.frame_id = "/camera_init";
        pubEachFrameLaserCloud.publish(eachPointCloudOutMsg);

//        sensor_msgs::PointCloud2 eachPointCloudOutMsg;
//        pcl::toROSMsg(lipmatch.laserCloudOri_m1, eachPointCloudOutMsg);
//        eachPointCloudOutMsg.header.stamp = ros::Time().fromSec(timeLaserOdometry);
//        eachPointCloudOutMsg.header.frame_id = "/camera_init";
//        pubEachFrameLaserCloud.publish(eachPointCloudOutMsg);


        nav_msgs::Odometry odomAftMapped;
        odomAftMapped.header.frame_id = "/camera_init";
        odomAftMapped.child_frame_id = "/aft_mapped";
        odomAftMapped.header.stamp = ros::Time().fromSec(timeLaserOdometry);
        odomAftMapped.pose.pose.orientation.x = q_w_curr.x();
        odomAftMapped.pose.pose.orientation.y = q_w_curr.y();
        odomAftMapped.pose.pose.orientation.z = q_w_curr.z();
        odomAftMapped.pose.pose.orientation.w = q_w_curr.w();
        odomAftMapped.pose.pose.position.x = t_w_curr.x();
        odomAftMapped.pose.pose.position.y = t_w_curr.y();
        odomAftMapped.pose.pose.position.z = t_w_curr.z();
        pubOdomAftMapped.publish(odomAftMapped);

        geometry_msgs::PoseStamped laserAfterMappedPose;
        laserAfterMappedPose.header = odomAftMapped.header;
        laserAfterMappedPose.pose = odomAftMapped.pose.pose;
        laserAfterMappedPath.header.stamp = odomAftMapped.header.stamp;
        laserAfterMappedPath.header.frame_id = "/camera_init";
        laserAfterMappedPath.poses.push_back(laserAfterMappedPose);
        pubLaserAfterMappedPath.publish(laserAfterMappedPath);

        static tf::TransformBroadcaster br;
        tf::Transform transform;
        tf::Quaternion q1;
        transform.setOrigin(tf::Vector3(t_w_curr(0), t_w_curr(1), t_w_curr(2)));
        q1.setW(q_w_curr.w());
        q1.setX(q_w_curr.x());
        q1.setY(q_w_curr.y());
        q1.setZ(q_w_curr.z());
        transform.setRotation(q1);
        br.sendTransform(tf::StampedTransform(transform, odomAftMapped.header.stamp, "/camera_init", "/aft_mapped"));

        r.sleep();
    }

    lipmatch.genPlaneMap();
    lipmatch.genCylinderMap();
    lipmatch.genVehicleMap();


    sensor_msgs::PointCloud2 matchedCloudOutMsg4;
    pcl::toROSMsg(lipmatch.laserCloudOri_m2, matchedCloudOutMsg4);
    matchedCloudOutMsg4.header.stamp = ros::Time().fromSec(timeLaserOdometry);
    matchedCloudOutMsg4.header.frame_id = "/camera_init";
    pubMatchedPoints2.publish(matchedCloudOutMsg4);

}



int main(int argc, char **argv)
{
    ros::init(argc, argv, "laserMapping");
    ros::NodeHandle nh;

//	float planeRes = 0.8;
    downSizeFilterKF.setLeafSize(0.2,0.2,0.2);

    pubLaserCloudFullRes = nh.advertise<sensor_msgs::PointCloud2>("/velodyne_cloud_registered", 300);

    pubOdomAftMapped = nh.advertise<nav_msgs::Odometry>("/aft_mapped_to_init", 300);

    pubLaserAfterMappedPath = nh.advertise<nav_msgs::Path>("/aft_mapped_path", 300);

    pubMatchedPoints1 = nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_matchedpoints1", 300);

    pubMatchedPoints2 = nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_matchedpoints2", 300);

    pubEachFrameLaserCloud = nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_each_frame", 300);

    tools::m_keyframe tk;

    m_keyframe_of_updating_list.push_back(tk);

    process();

    ros::spin();

    return 0;
}