#include "flightsession.h"
#include "exiv2/exiv2.hpp"
#include <iostream>

using namespace boost::filesystem;
using std::cout;
using std::vector;
using cv::DMatch;
using cv::Mat;
using cv::KeyPoint;

FlightSession::FlightSession(string imageDirectory) : imageDirectory(imageDirectory)
{
	vector<directory_entry> v;
	assert(is_directory(imageDirectory));
	this->imageDirectoryPath = path(imageDirectory);

	this->imageFeaturesPath = this->imageDirectoryPath / "features";
	this->imageMatchesPath = this->imageDirectoryPath / "matches";
	this->imageTracksPath = this->imageDirectoryPath / "tracks";

	if (!boost::filesystem::exists(this->imageFeaturesPath) || !boost::filesystem::exists(this->imageMatchesPath)
	 || !boost::filesystem::exists(this->imageTracksPath));
	{
		cout << "Creating directory "<< this->imageFeaturesPath.string()<<endl;
		boost::filesystem::create_directory(this->imageFeaturesPath);
		cout <<  "Creating directory "<<this->imageMatchesPath.string()<<endl;
		boost::filesystem::create_directory(this->imageMatchesPath);
		cout << "Creating directory "<<this->imageTracksPath.string();
		boost::filesystem::create_directory(this->imageTracksPath);
	}
	copy_if(
		directory_iterator(imageDirectory),
		directory_iterator(),
		back_inserter(v),
		[](const directory_entry &e) {
			return is_regular_file(e);
		});
	for (auto entry : v)
	{
		Img img;
		img.fileName = parseFileNameFromPath(entry.path().string());
		auto loc = this->getCoordinates(entry.path().string());
		img.location = loc;
		this->imageData.push_back(img);
	}
	cout << "Found " << this->imageData.size() << " usable images" << endl;
}

Location FlightSession::getCoordinates(string imagePath)
{
	Location loc;
	Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(imagePath);
	assert(image.get() != 0);
	image->readMetadata();
	Exiv2::ExifData &exifData = image->exifData();
	if (exifData.empty())
	{
		std::string error(imagePath);
		error += ": No Exif data found in the file";
		throw Exiv2::Error(1, error);
	}

	Exiv2::ExifData::const_iterator end = exifData.end();
	Exiv2::Value::AutoPtr latV = Exiv2::Value::create(Exiv2::signedRational);
	Exiv2::Value::AutoPtr longV = Exiv2::Value::create(Exiv2::signedRational);
	auto longitudeKey = Exiv2::ExifKey("Exif.GPSInfo.GPSLongitude");
	auto latitudeKey = Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude");
	auto latPos = exifData.findKey(latitudeKey);
	auto longPos = exifData.findKey(longitudeKey);
	if (latPos == exifData.end() || longPos == exifData.end())
		throw Exiv2::Error(1, "Key not found");
	// Get a pointer to a copy of the value
	latV = latPos->getValue();
	longV = longPos->getValue();
	auto latitude = latV->toFloat() + (latV->toFloat(1) / 60.0) + (latV->toFloat(2) / 3600.0);
	auto longitude = longV->toFloat() + (longV->toFloat(1) / 60.0) + (longV->toFloat(2) / 3600.0);

	loc.longitude = longitude;
	loc.latitude = latitude;
	return loc;
}

vector<Img> FlightSession::getImageSet() const
{
	return this->imageData;
}

const path FlightSession::getImageDirectoryPath() const
{
	return this->imageDirectoryPath;
}

const path FlightSession::getImageFeaturesPath() const
{
	return this->imageFeaturesPath;
}

const path FlightSession::getImageMatchesPath() const {
	return this->imageMatchesPath;
}

/*
bool FlightSession::saveTracksFile(std::map <int, std::vector <int>> tracks) {
    if (! tracks.size())
        return false;
    
    auto imageTracksFile = this->getImageTracksPath() / "tracks.xml";
    cv::FileStorage fs(imageTracksFile.string(), cv::FileStorage::WRITE);
    fs << "Tracks" << tracks;
    return boost::filesystem::exists(imageTracksFile);
}
*/

const path FlightSession::getImageTracksPath() const {
	return this->imageTracksPath;
}

int FlightSession::getImageIndex(string imageName) const{
	for(int i=0; i < this->imageData.size(); ++i) {
		if (this->imageData[i].fileName == imageName) {
			return i;
		}
	}
	return -1;
}

bool FlightSession::saveImageFeaturesFile(string imageName, const std::vector<cv::KeyPoint>& keypoints, const cv::Mat descriptors) {
    auto imageFeaturePath = this->getImageFeaturesPath() / imageName;
    if (!boost::filesystem::exists(imageFeaturePath))
    {
        cv::FileStorage file(imageFeaturePath.string(), cv::FileStorage::WRITE);
        file << "Keypoints " << keypoints;
        file << "Descriptors " << descriptors;
        file.release();
    }
    return boost::filesystem::exists(imageFeaturePath);
}

bool FlightSession::saveMatches(string fileName, std::vector<cv::DMatch> matches) {

    auto imageMatchesPath = this->getImageMatchesPath() / (fileName + ".xml");
    cout << "Writing file "<< imageMatchesPath.string()<<endl;;
    cv::FileStorage fs(imageMatchesPath.string(), cv::FileStorage::WRITE);
    fs << "MatchCount" << (int)matches.size();
    fs << "Matches" << matches;
    fs.release();
    return boost::filesystem::exists(imageMatchesPath);
}

vector <cv::DMatch> FlightSession:: loadMatches(string fileName) {
    auto imageMatchesPath = this->getImageMatchesPath() / (fileName + ".xml");
    cv::FileStorage fs(imageMatchesPath.string(), cv::FileStorage::READ);
    vector<DMatch> matches;
    fs["Matches"] >> matches;
    return matches;
}

ImageFeatures FlightSession::loadFeatures(string imageName) {
	auto imageFeaturePath = this->getImageFeaturesPath() / imageName;
	cv::FileStorage fs(imageFeaturePath.string(), cv::FileStorage::READ);
    vector <KeyPoint> keypoints;
	Mat descriptors;
    fs["Keypoints"] >> keypoints;
	fs["Descriptors"] >> descriptors;

    return make_pair(keypoints, descriptors);
}