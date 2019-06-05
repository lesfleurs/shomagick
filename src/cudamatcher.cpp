#include "cudamatcher.hpp"


CUDARobustMatcher::~CUDARobustMatcher()
{

}

void CUDARobustMatcher::computeKeyPoints(const cv::Mat &image, std::vector<cv::KeyPoint> &keypoints)
{
    detector_->detect(image, keypoints);
}

void  CUDARobustMatcher::computeDescriptors(const cv::Mat &image, std::vector<cv::KeyPoint> &keypoints, cv::Mat &descriptors)
{
    extractor_->compute(image, keypoints, descriptors);
}

int CUDARobustMatcher::ratioTest(std::vector<std::vector<cv::DMatch>> &matches)
{
    int removed = 0;
    // for all matches
    for (std::vector<std::vector<cv::DMatch>>::iterator
        matchIterator = matches.begin();
        matchIterator != matches.end(); ++matchIterator)
    {
        // if 2 NN has been identified
        if (matchIterator->size() > 1)
        {
            // check distance ratio
            if ((*matchIterator)[0].distance / (*matchIterator)[1].distance > ratio_)
            {
                matchIterator->clear(); // remove match
                removed++;
            }
        }
        else
        {                         // does not have 2 neighbours
            matchIterator->clear(); // remove match
            removed++;
        }
    }
    return removed;
}

void  CUDARobustMatcher::symmetryTest(const std::vector<std::vector<cv::DMatch>> &matches1,
    const std::vector<std::vector<cv::DMatch>> &matches2,
    std::vector<cv::DMatch> &symMatches)
{

    // for all matches image 1 -> image 2
    for (std::vector<std::vector<cv::DMatch>>::const_iterator
        matchIterator1 = matches1.begin();
        matchIterator1 != matches1.end(); ++matchIterator1)
    {

        // ignore deleted matches
        if (matchIterator1->empty() || matchIterator1->size() < 2)
            continue;

        // for all matches image 2 -> image 1
        for (std::vector<std::vector<cv::DMatch>>::const_iterator
            matchIterator2 = matches2.begin();
            matchIterator2 != matches2.end(); ++matchIterator2)
        {
            // ignore deleted matches
            if (matchIterator2->empty() || matchIterator2->size() < 2)
                continue;

            // Match symmetry test
            if ((*matchIterator1)[0].queryIdx ==
                (*matchIterator2)[0].trainIdx &&
                (*matchIterator2)[0].queryIdx ==
                (*matchIterator1)[0].trainIdx)
            {
                // add symmetrical match
                symMatches.push_back(
                    cv::DMatch((*matchIterator1)[0].queryIdx,
                    (*matchIterator1)[0].trainIdx,
                        (*matchIterator1)[0].distance));
                break; // next match in image 1 -> image 2
            }
        }
    }
}

void  CUDARobustMatcher::robustMatch(const cv::Mat &image1, const cv::Mat &trainImage, std::vector<cv::DMatch> &matches,
    std::vector<cv::KeyPoint> &keypoints1, std::vector<cv::KeyPoint> &keypoints2)
{

    // 1a. Detection of the ORB features
    computeKeyPoints(image1, keypoints1);
    computeKeyPoints(trainImage, keypoints2);

    // 1b. Extraction of the ORB descriptors
    cv::Mat descriptors1;
    cv::Mat descriptors2;
    computeDescriptors(image1, keypoints1, descriptors1);
    computeDescriptors(trainImage, keypoints2, descriptors2);

    robustMatch(descriptors1, descriptors2, matches);
}

void  CUDARobustMatcher::robustMatch(
    const cv::Mat descriptors1,
    const cv::Mat descriptors2,
    std::vector<cv::DMatch>& matches
)
{
    cv::cuda::GpuMat gDescriptors1, gDescriptors2;
    gDescriptors1.upload(descriptors1);
    gDescriptors2.upload(descriptors2);
    // 2. Match the two image descriptors
    std::vector<std::vector<cv::DMatch>> matches12, matches21;

    
    // 2a. From image 1 to image 2
    matcher_->knnMatch(gDescriptors1, gDescriptors2, matches12, 2); // return 2 nearest neighbours 

    // 2b. From image 2 to image 1
    matcher_->knnMatch(gDescriptors2, gDescriptors1, matches21, 2); // return 2 nearest neighbours
    // 3. Remove matches for which NN ratio is > than threshold
    // clean image 1 -> image 2 matches
    ratioTest(matches12);
    // clean image 2 -> image 1 matches
    ratioTest(matches21);

    // 4. Remove non-symmetrical matches
    symmetryTest(matches12, matches21, matches);
}

void  CUDARobustMatcher::fastRobustMatch(const cv::Mat descriptors1, const cv::Mat descriptors2, std::vector<cv::DMatch> &matches)
{
    matches.clear();
    std::vector<std::vector<cv::DMatch>> matches12;

    // 2. Match the two image descriptors
    std::vector<std::vector<cv::DMatch>> kmatches;
    matcher_->knnMatch(descriptors1, descriptors2, kmatches, 2);

    // 3. Remove matches for which NN ratio is > than threshold
    ratioTest(kmatches);

    // 4. Fill good matches container
    for (std::vector<std::vector<cv::DMatch>>::iterator
        matchIterator = kmatches.begin();
        matchIterator != kmatches.end(); ++matchIterator)
    {
        if (!matchIterator->empty())
            matches.push_back((*matchIterator)[0]);
    }
}
