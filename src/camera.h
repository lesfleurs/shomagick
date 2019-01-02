#ifndef CAMERA_HPP_
#define CAMERA_HPP_

#include <opencv2/opencv.hpp>
#include <opengv/types.hpp>
#include <ostream>

class Pose {
  private:
    cv::Mat rotation;
    cv::Mat translation;

  public:
    Pose () : rotation(cv::Mat::zeros(3,1,CV_32F)), translation(cv::Mat::zeros(3,1,CV_32F)) {}
    Pose(cv::Mat rotation, cv::Mat translation) : rotation(rotation), translation(translation) {}
    cv::Mat getRotationMatrix() const;
    cv::Mat getOrigin() const;
    friend std::ostream & operator << (std::ostream &out, const Pose &p); 
};

class Camera
{

  private:
    cv::Mat cameraMatrix;
    cv::Mat distortionCoefficients;
    void _cvPointsToBearingVec(cv::Mat pRect, opengv::bearingVectors_t& );

  public:
    Camera();
    Camera(cv::Mat cameraMatrix, cv::Mat distortion);
    double getFocal();
    cv::Mat getKMatrix();
    cv::Mat getDistortionMatrix();
    void cvPointsToBearingVec(
    const std::vector<cv::Point2f>&, opengv::bearingVectors_t& );
    opengv::bearingVector_t  cvPointToBearingVec(cv::Point2f &point);
};
#endif