#pragma once
#include <math.h>

#ifdef CV_LOAD_IMAGE_COLOR
#   define SHO_LOAD_COLOR_IMAGE_OPENCV_ENUM    CV_LOAD_IMAGE_COLOR
#   define SHO_LOAD_ANYDEPTH_IMAGE_OPENCV_ENUM CV_LOAD_IMAGE_ANYDEPTH
#   define SHO_BGR2RGB                         CV_BGR2RGB
#   define SHO_GRAYSCALE                       CV_LOAD_IMAGE_GRAYSCALE
#else
#   define SHO_LOAD_COLOR_IMAGE_OPENCV_ENUM    cv::IMREAD_COLOR
#   define SHO_LOAD_ANYDEPTH_IMAGE_OPENCV_ENUM cv::IMREAD_ANYDEPTH
#   define SHO_BGR2RGB                         cv::COLOR_BGR2RGB
#   define SHO_GRAYSCALE                       cv::IMREAD_GRAYSCALE
#endif



#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif