#include "PUTSLAM/PUTSLAM.h"
#include "Utilities/simulator.h"
#include "Utilities/stopwatch.h"
#include "MotionModel/decayingVelocityModel.h"


#include <assert.h>

PUTSLAM::PUTSLAM() {

    loadConfigs();

	// Reading robot starting pose
    VOPoseEstimate = grabber->getStartingSensorPose();

	// File to save trajectory
	trajectoryFreiburgStream.open("VO_trajectory.res");
	trajectoryMotionModelStream.open("MotionModel_trajectory.res");
	drawImages = false;
	visualize = false;
    map->setDrawOptions(false);
#ifdef BUILD_WITH_ROS
		//////////////////////////////////////////////////////////////////////ROS
        workWithROS = false;
#endif
}

void PUTSLAM::moveMapFeaturesToLocalCordinateSystem(const Mat34& cameraPose,
		std::vector<MapFeature>& mapFeatures) {

	for (std::vector<MapFeature>::iterator it = mapFeatures.begin();
			it != mapFeatures.end(); ++it) {

		// Feature position in GCS
		Mat34 featurePos((*it).position);

		// Moving feature from GCS to LCS
		featurePos = (cameraPose.inverse()).matrix() * featurePos.matrix();
		it->position = Vec3(featurePos(0, 3), featurePos(1, 3),
				featurePos(2, 3));

		// Feature projection onto image
		Eigen::Vector3d projectedMapPoint =
				map->getDepthSensorModel().inverseModel(featurePos(0, 3),
						featurePos(1, 3), featurePos(2, 3));

		// Saving image position
		it->u = projectedMapPoint[0];
		it->v = projectedMapPoint[1];
	}
}

bool PUTSLAM::removeCloseFeatures(std::vector<RGBDFeature> &existingFeatures,
		Eigen::Vector3f feature3D, cv::Point2f feature2D, double minEuclideanDistanceOfFeatures, double minImageDistanceOfFeatures) {

	for (auto &existingFeature : existingFeatures) {

		Eigen::Vector3f tmp((float) existingFeature.position.x(),
				(float) existingFeature.position.y(),
				(float) existingFeature.position.z());
		float norm = (tmp - feature3D).norm();
		if (norm < minEuclideanDistanceOfFeatures) {
			return false;
		}

		cv::Point2f point((float)existingFeature.u, (float)existingFeature.v);
		float imageNorm = (float) cv::norm(point - feature2D);
		if (imageNorm < minImageDistanceOfFeatures) {
			return false;
		}
	}
	return true;
}


bool PUTSLAM::removeCloseFeatures(const std::vector<MapFeature> &existingFeatures,
		Eigen::Vector3f feature3D, cv::Point2f feature2D, double minEuclideanDistanceOfFeatures, double minImageDistanceOfFeatures) {

	for (auto &existingFeature : existingFeatures) {

		Eigen::Vector3f tmp((float) existingFeature.position.x(),
				(float) existingFeature.position.y(),
				(float) existingFeature.position.z());
		float norm = (tmp - feature3D).norm();
		if (norm < minEuclideanDistanceOfFeatures) {
			return false;
		}

		cv::Point2f point((float)existingFeature.u,(float) existingFeature.v);
		float imageNorm = (float) cv::norm(point - feature2D);
		if (imageNorm < minImageDistanceOfFeatures) {
			return false;
		}
	}
	return true;
}

int PUTSLAM::chooseFeaturesToAddToMap(const Matcher::featureSet& features,
		int addedCounter, int maxOnceFeatureAdd,
		const std::vector<MapFeature>& mapFeatures,
		float minEuclideanDistanceOfFeatures, float minImageDistanceOfFeatures,
		int cameraPoseId, std::vector<RGBDFeature>& mapFeaturesToAdd) {

	assert(
			("chooseFeaturesToAddToMap", features.feature3D.size()
					== features.undistortedFeature2D.size()));
	assert(
			("chooseFeaturesToAddToMap 2", features.feature3D.size()
					== features.descriptors.rows));

	// Lets process possible features to add
	for (unsigned int j = 0;
			j < features.feature3D.size() && addedCounter < maxOnceFeatureAdd;
			j++) {

		// We only add features of proper depth
		if (features.feature3D[j][2] > 0.8 && features.feature3D[j][2] < 6.0) {

			// Assume that the feature is ok
			bool featureOk = true;

			// Lets remove features too close to existing features
			featureOk = removeCloseFeatures(mapFeatures,
									features.feature3D[j], features.undistortedFeature2D[j],
									minEuclideanDistanceOfFeatures,
									minImageDistanceOfFeatures);

			// Lets remove features too close to features to add :)
			if (featureOk) {

				featureOk = removeCloseFeatures(mapFeaturesToAdd,
						features.feature3D[j], features.undistortedFeature2D[j],
						minEuclideanDistanceOfFeatures,
						minImageDistanceOfFeatures);
			}

			if (featureOk) {
				// Create an extended descriptor
				cv::Mat descMat;


				if (!features.descriptors.empty()) {
					descMat = features.descriptors.row(j).clone();
				}
				else {
					// TODO: should we compute the descriptor every time?
					std::cout<<"Missing descriptor - BUG!" << std::endl;
					exit(0);
				}


				ExtendedDescriptor desc(features.distortedFeature2D[j],
						features.undistortedFeature2D[j],
						Vec3(features.feature3D[j].x(), features.feature3D[j].y(), features.feature3D[j].z()),
						descMat,
						features.feature2D[j].octave,
						features.detDist[j]);

				// In further processing we expect more descriptors
				std::map<unsigned int, ExtendedDescriptor> extDescriptors { {cameraPoseId, desc} };

				// Convert translation
				Eigen::Translation<double, 3> featurePosition(
						features.feature3D[j].cast<double>());

				// Add to set added later to map
				RGBDFeature f(featurePosition,
						features.undistortedFeature2D[j].x,
						features.undistortedFeature2D[j].y, extDescriptors);
				mapFeaturesToAdd.push_back(f);

				addedCounter++;

			}
		}
	}
	return addedCounter;
}

/// PUBLIC

/// Current Pose
void PUTSLAM::getCurrentPose(Mat34& camPose)
{
    camPose = map->getSensorPose();
}

///Current Frame
void PUTSLAM::getCurrentFrame(cv::Mat& RGBD, cv::Mat& depthImg)
{
    getFrameEvent.lock();
    RGBD=RGBDimg;
    depthImg=depthImgimg;
    getFrameEvent.unlock();
}

#ifdef BUILD_PUTSLAM_VISUALIZER
///Attach visualizer
void PUTSLAM::attachVisualizer(QGLVisualizer* visualizer) {
	((FeaturesMap*) map)->attach(visualizer);
}
#endif

void PUTSLAM::createAndSaveOctomap(double depthImageScale) {
	int size = map->getPoseCounter();
	// We process every octomapCloudStepSize cloud
	for (int i = 0; i < size; i = i + octomapCloudStepSize) {

		std::cout << "Octomap uses point cloud with id = " << i << std::endl;

		// Getting pose and images
		cv::Mat rgbImage, depthImage;
		map->getImages(i, rgbImage, depthImage);
		Mat34 pose = map->getSensorPose(i);
		Eigen::Matrix4f tmpPose = Eigen::Matrix4f(pose.matrix().cast<float>());

		// Creating color cloud
		std::vector<std::pair<Eigen::Vector3f, Eigen::Vector3i>> colorPointCloud =
				RGBD::imageToColorPointCloud(rgbImage, depthImage,
						matcher->matcherParameters.cameraMatrixMat, tmpPose,
						depthImageScale);

		// We add every point
		for (unsigned int k = 0; k < colorPointCloud.size(); k++) {
			octomap::point3d endpoint((float) colorPointCloud[k].first.x(),
					(float) colorPointCloud[k].first.y(),
					(float) colorPointCloud[k].first.z());
			octomap::ColorOcTreeNode* n = octomapTree.get()->updateNode(
					endpoint,
					true);

			// Adding also color
			octomapTree.get()->integrateNodeColor(
					(float) colorPointCloud[k].first.x(),
					(float) colorPointCloud[k].first.y(),
					(float) colorPointCloud[k].first.z(),
					(uint8_t) colorPointCloud[k].second.x(),
					(uint8_t) colorPointCloud[k].second.y(),
					(uint8_t) colorPointCloud[k].second.z());
		}

	}
	// We update the colors
	std::cout << "Updating tree color" << std::endl;
	octomapTree.get()->updateInnerOccupancy();
	// Writing octomap to file
	std::string filename(octomapFileToSave);
	std::cout << "Writing color tree to " << filename << std::endl;
	// write color tree
	octomapTree.get()->write(filename);
}

void PUTSLAM::createAndSaveOctomapOffline(double depthImageScale) {
	std::ifstream reconstructStr("reconstruction.res");

	int i = 0;
	while (1) {
		bool middleOfSequence = grabber->grab(); // grab frame
		if (!middleOfSequence)
			break;

		SensorFrame currentSensorFrame = grabber->getSensorFrame();
		float timeS, tx, ty, tz, qw, qx, qy, qz;
		reconstructStr >> timeS >> tx >> ty >> tz >> qx >> qy >> qz >> qw;

		if (i % octomapCloudStepSize == 0) {
			std::cout << "Octomap uses point clouds with id = " << i
					<< std::endl;
			Eigen::Quaternion<float> Q(qw, qx, qy, qz);
			Eigen::Matrix4f tmpPose;
			tmpPose.setIdentity();
			tmpPose.block<3, 3>(0, 0) = Q.toRotationMatrix();
			tmpPose(0, 3) = tx;
			tmpPose(1, 3) = ty;
			tmpPose(2, 3) = tz;

			// Save for octomap
			std::vector<std::pair<Eigen::Vector3f, Eigen::Vector3i>> colorPointCloud =
					RGBD::imageToColorPointCloud(currentSensorFrame.rgbImage,
							currentSensorFrame.depthImage,
							matcher->matcherParameters.cameraMatrixMat, tmpPose,
							depthImageScale);

			for (unsigned int k = 0; k < colorPointCloud.size(); k++) {
				octomap::point3d endpoint((float) colorPointCloud[k].first.x(),
						(float) colorPointCloud[k].first.y(),
						(float) colorPointCloud[k].first.z());
				octomap::ColorOcTreeNode* n = octomapTree.get()->updateNode(
						endpoint, true);

				octomapTree.get()->integrateNodeColor(
						(float) colorPointCloud[k].first.x(),
						(float) colorPointCloud[k].first.y(),
						(float) colorPointCloud[k].first.z(),
						(uint8_t) colorPointCloud[k].second.x(),
						(uint8_t) colorPointCloud[k].second.y(),
						(uint8_t) colorPointCloud[k].second.z());
			}

		}
		i++;
	}

	// set inner node colors
	std::cout << "Updating tree color" << std::endl;
	octomapTree.get()->updateInnerOccupancy();

	std::string filename(octomapFileToSave);
	std::cout << "Writing color tree to " << filename << std::endl;

	// write color tree
	octomapTree.get()->write(filename);
}

void PUTSLAM::processFirstFrame(SensorFrame &currentSensorFrame,
		int &cameraPoseId) {
	matcher->Matcher::detectInitFeatures(currentSensorFrame);
    VOFeaturesSizeLog.push_back(matcher->getNumberOfFeatures());

	// cameraPose as Eigen::Transform
	Mat34 cameraPose = Mat34(VOPoseEstimate.cast<double>());

	// Add new position to the map
	if (!onlyVO) {
		cameraPoseId = map->addNewPose(cameraPose, currentSensorFrame.timestamp,
				currentSensorFrame.rgbImage, currentSensorFrame.depthImage);
	}

	// Correct motionModel
//	motionModelLastTime = currentSensorFrame.timestamp;
//	Mat34 x = motionModel.get()->correct(cameraPose);
//	motionModelPose = Eigen::Matrix4f(x.matrix().cast<float>());
}

//play trajectory
void PUTSLAM::startPlaying(std::string trajectoryFilename, int delayPlay) {
	///for inverse SLAM problem
	Simulator simulator;
	simulator.loadTrajectory(trajectoryFilename);
	std::vector<Mat34> traj = simulator.getTrajectory();
	int trajIt = 0;

	// Main loop
	while (true) {
		bool middleOfSequence = grabber->grab(); // grab frame
		if (!middleOfSequence)
			break;
		///for inverse SLAM problem
		if (trajIt > (int)traj.size() - 1)
			break;
		SensorFrame currentSensorFrame = grabber->getSensorFrame();


        getFrameEvent.lock();
        RGBDimg=currentSensorFrame.rgbImage;
        depthImgimg=currentSensorFrame.depthImage;
        getFrameEvent.unlock();
		if (drawImages) {
			cv::imshow("PUTSLAM RGB frame", currentSensorFrame.rgbImage);
			cv::imshow("PUTSLAM Depth frame", currentSensorFrame.depthImage);
			cv::waitKey(5000);
		}
		if (trajIt == 0) {
			// cameraPose as Eigen::Transform
			Mat34 cameraPose = Mat34(VOPoseEstimate.cast<double>());

			// Add new position to the map
			map->addNewPose(cameraPose, currentSensorFrame.timestamp,
					currentSensorFrame.rgbImage, currentSensorFrame.depthImage);

		} else {
			Eigen::Matrix4f transformation;

			//for inverse slam problem
			Mat34 transReal = traj[trajIt - 1].inverse() * traj[trajIt];
			transformation = transReal.cast<float>().matrix();
			std::cout << "iteration: " << trajIt << "\n";

			VOPoseEstimate = VOPoseEstimate * transformation;

			// cameraPose as Eigen::Transform
			Mat34 cameraPoseIncrement = Mat34(transformation.cast<double>());

			// Add new position to the map
			map->addNewPose(cameraPoseIncrement, currentSensorFrame.timestamp,
					currentSensorFrame.rgbImage, currentSensorFrame.depthImage);
		}
		usleep(delayPlay);
		trajIt++;
	}

	std::cout << "Job finished! Good bye :)" << std::endl;
}

/// set drawing options
void PUTSLAM::setDrawOptions(bool _draw, bool _drawImages) {
	visualize = _draw;
	drawImages = _drawImages;
	map->setDrawOptions(visualize);
}

// At beggining
void PUTSLAM::readingSomeParameters() {

	addFeaturesWhenMapSizeLessThan =
			((FeaturesMap*) map)->getAddFeaturesWhenMapSizeLessThan();
	addFeaturesWhenMeasurementSizeLessThan =
			((FeaturesMap*) map)->getAddFeaturesWhenMeasurementSizeLessThan();
	maxOnceFeatureAdd = ((FeaturesMap*) map)->getMaxOnceFeatureAdd();
	minEuclideanDistanceOfFeatures =
			((FeaturesMap*) map)->getMinEuclideanDistanceOfFeatures();
	minImageDistanceOfFeatures =
            ((FeaturesMap*) map)->getMinImageDistanceOfFeatures();
    addNoFeaturesWhenMapSizeGreaterThan =
            ((FeaturesMap*) map)->getAddNoFeaturesWhenMapSizeGreaterThan();
    maxMeasurementsToAddPoseToPoseEdge = ((FeaturesMap*) map)->getMaxMeasurementsToAddPoseToPoseEdge();

    minMeasurementsToAddPoseToFeatureEdge =
			((FeaturesMap*) map)->getMinMeasurementsToAddPoseToFeatureEdge();

	addPoseToPoseEdges = ((FeaturesMap*) map)->getAddPoseToPoseEdges();

	depthImageScale = ((FileGrabber*) grabber)->parameters.depthImageScale;

	getVisibleFeaturesGraphMaxDepth = 5000;
	getVisibleFeatureDistanceThreshold = 15.0;
}

void PUTSLAM::initialization() {
	// Optimize during trajectory acquisition
    if (optimizationThreadVersion == OPTTHREAD_ON)
        map->startOptimizationThread(1, 0);
	else if (optimizationThreadVersion == OPTTHREAD_ON_ROBUSTKERNEL)
		map->startOptimizationThread(1, 0, "Cauchy", 1);

	// Thread looking for too close features
	if (mapManagmentThreadVersion == MAPTHREAD_ON)
		map->startMapManagerThread(1);

    // thread for geometric loop closure
	if (loopClosureThreadVersion == LCTHREAD_ON)
		map->startLoopClosureThread(1, loopClosureMatcher);

	// Creating octomap
	if (octomap > 0)
		octomapTree.reset(new octomap::ColorOcTree(octomapResolution));

	if (octomapOffline > 0) {
		createAndSaveOctomapOffline(depthImageScale);
		exit(0);
	}
}

void PUTSLAM::loadConfigs() {
    tinyxml2::XMLDocument config;
    config.LoadFile("../../resources/putslamconfigGlobal.xml");
	if (config.ErrorID())
		std::cout << "unable to load config file.\n";

	config.FirstChildElement("PUTSLAM")->QueryIntAttribute("verbose", &verbose);
	config.FirstChildElement("PUTSLAM")->QueryIntAttribute("onlyVO", &onlyVO);
	config.FirstChildElement("PUTSLAM")->QueryBoolAttribute("keepCameraFrames",
			&keepCameraFrames);
	config.FirstChildElement("PUTSLAM")->QueryIntAttribute("octomap", &octomap);
	config.FirstChildElement("PUTSLAM")->QueryDoubleAttribute(
			"octomapResolution", &octomapResolution);
	config.FirstChildElement("PUTSLAM")->QueryIntAttribute(
			"octomapCloudStepSize", &octomapCloudStepSize);
	octomapFileToSave = config.FirstChildElement("PUTSLAM")->Attribute(
			"octomapFileToSave");
	config.FirstChildElement("PUTSLAM")->QueryIntAttribute("octomapOffline",
			&octomapOffline);
	if (!keepCameraFrames && octomap)
		throw std::runtime_error(
				std::string(
						"Camera frames are not used (keepCameraFrames==false). Octomap is not available.\nModify config files.\n"));

	// Thread settings
	config.FirstChildElement("ThreadSettings")->QueryIntAttribute("verbose",
            &verbose);
	config.FirstChildElement("ThreadSettings")->QueryIntAttribute(
			"optimizationThreadVersion", &optimizationThreadVersion);
	config.FirstChildElement("ThreadSettings")->QueryIntAttribute(
			"mapManagmentThreadVersion", &mapManagmentThreadVersion);
	config.FirstChildElement("ThreadSettings")->QueryIntAttribute(
			"loopClosureThreadVersion", &loopClosureThreadVersion);

	if (verbose > 0) {
		std::cout << "PUTSLAM: optimizationThreadVersion = "
				<< optimizationThreadVersion << std::endl;
		std::cout << "PUTSLAM: mapManagmentThreadVersion = "
				<< mapManagmentThreadVersion << std::endl;
	}

	// Create map
	if (verbose > 0) {
		std::cout << "Getting grabber config" << std::endl;
	}
	std::string configFileGrabber(
			config.FirstChildElement("Grabber")->FirstChildElement(
					"calibrationFile")->GetText());

	if (verbose > 0) {
		std::cout << "Getting map config" << std::endl;
	}
	std::string configFileMap(
			config.FirstChildElement("Map")->FirstChildElement("parametersFile")->GetText());

	if (verbose > 0) {
		std::cout << "Creating features map" << std::endl;
    }
    map = createFeaturesMap(configFileMap, configFileGrabber);
	map->setStoreImages(keepCameraFrames);

	if (verbose > 0) {
		std::cout << "Features map is initialized" << std::endl;
	}

	std::string grabberType(
			config.FirstChildElement("Grabber")->FirstChildElement("name")->GetText());

	std::string grabberConfigFile(
			config.FirstChildElement("Grabber")->FirstChildElement(
					"calibrationFile")->GetText());

	if (verbose > 0) {
		std::cout << "Creating grabber with type = " << grabberType
				<< std::endl;
	}

    if (grabberType == "Kinect") {
        grabber = createGrabberKinect(grabberConfigFile, Grabber::MODE_BUFFER);
    } else if (grabberType == "Xtion") {
		grabber = createGrabberXtion(grabberConfigFile, Grabber::MODE_BUFFER);
    }
#ifdef BUILD_WITH_ROS
	////////////////////////////////////////////////////////////ROS
	else if (grabberType == "ROS") {
		std::cout<<"\n create ROS Grabber \n";
		grabber = createGrabberROS(nh);
	}
#endif
	/// Still do not take into account the config file
    else if (grabberType == "File") {
		grabber = createGrabberFile(grabberConfigFile);
    } else if (grabberType == "MesaImaging")
        grabber = createGrabberKinect();
    else
		// Default
        grabber = createGrabberKinect();

	// create objects and print configuration
	if (verbose > 0) {
        std::cout << "Current grabber: " << grabber->getName() << std::endl;
	}
    std::string matcherParameters =
			config.FirstChildElement("Matcher")->FirstChildElement(
					"parametersFile")->GetText();
    std::string matcherParametersLC =
			config.FirstChildElement("Matcher")->FirstChildElement(
					"parametersFileLC")->GetText();

	if (verbose > 0) {
		std::cout << "Creating matcher" << std::endl;
	}
	matcher = createMatcherOpenCV(matcherParameters, grabberConfigFile);
	if (verbose > 0) {
        std::cout << "Current matcher: " << matcher->getName() << std::endl;
	}
	loopClosureMatcher = createloopClosingMatcherOpenCV(matcherParametersLC,
			grabberConfigFile);
	if (verbose > 0) {
        std::cout << "Loop closure current matcher: " << matcher->getName()
				<< std::endl;
	}
}

Eigen::Matrix4f PUTSLAM::runVO(SensorFrame &currentSensorFrame, std::vector<cv::DMatch> &inlierMatches) {
	Stopwatch<> voTime;
	Eigen::Matrix4f transformation;
	double inlierRatio = matcher->Matcher::runVO(currentSensorFrame,
			transformation, inlierMatches);
	voTime.stop();
	timeMeasurement.voTimes.push_back((long int)voTime.elapsed());

	VORansacInlierRatioLog.push_back(inlierRatio);
	VOFeaturesSizeLog.push_back(matcher->Matcher::getNumberOfFeatures());
	return transformation;
}

void PUTSLAM::addPoseToMap(SensorFrame &currentSensorFrame, Eigen::Matrix4f &poseIncrement, int &cameraPoseId ) {
	// cameraPose as Eigen::Transform
	Mat34 cameraPoseIncrement = Mat34(poseIncrement.cast<double>());

	// Add new position to the map
	Stopwatch<> tmp;
	tmp.start();
	cameraPoseId = map->addNewPose(cameraPoseIncrement,
			currentSensorFrame.timestamp, currentSensorFrame.rgbImage,
			currentSensorFrame.depthImage);
	tmp.stop();
	timeMeasurement.mapAddNewPoseTimes.push_back((long int)tmp.elapsed());
}

Mat34 PUTSLAM::getMapPoseEstimate() {
	Stopwatch<> tmp;
	tmp.start();
	Mat34 cameraPose = map->getSensorPose();
	tmp.stop();
	timeMeasurement.mapGetSensorPoseTimes.push_back((long int)tmp.elapsed());
	return cameraPose;
}

Eigen::Matrix4f PUTSLAM::getPoseIncrementFromMap(int frameCounter) {

	if ( frameCounter > 1 ) {
		Eigen::Matrix4f first( map->getSensorPose(frameCounter-2).matrix().cast<float>() );
		Eigen::Matrix4f second( map->getSensorPose(frameCounter-1).matrix().cast<float>() );
		return first.inverse() * second;
	}
	return Eigen::Matrix4f::Identity();
}

std::vector<MapFeature> PUTSLAM::getAndFilterFeaturesFromMap(SensorFrame &currentSensorFrame, Mat34 cameraPose,std::vector<int> &frameIds,
std::vector<double> &angles ) {

	Stopwatch<> tmp;
	tmp.start();
    //std::vector<MapFeature> mapFeatures = map->getVisibleFeatures(cameraPose);
    std::vector<MapFeature> mapFeatures = map->getCovisibleFeatures();
	tmp.stop();
	timeMeasurement.mapGetVisibleFeaturesTimes.push_back((long int)tmp.elapsed());

	//mapFeatures = map->getVisibleFeatures(cameraPose, getVisibleFeaturesGraphMaxDepth, getVisibleFeatureDistanceThreshold);

	tmp.start();
	map->findNearestFrame(mapFeatures, frameIds, angles,
			matcher->matcherParameters.maxAngleBetweenFrames);
	tmp.stop();
	timeMeasurement.mapFindNearestFrameTimes.push_back((long int)tmp.elapsed());

	//Remove features that we do not have a good observation angle
	tmp.start();
	removeMapFeaturesWithoutGoodObservationAngle(mapFeatures, frameIds, angles);
	tmp.stop();
	timeMeasurement.mapRemoveMapFeaturesTimes.push_back((long int)tmp.elapsed());

	// Move mapFeatures to local coordinate system
	tmp.start();
	moveMapFeaturesToLocalCordinateSystem(cameraPose, mapFeatures);
	tmp.stop();
	timeMeasurement.mapMoveMapFeaturesToLCSTimes.push_back((long int)
			tmp.elapsed());

	// Now lets check if those features are not behind sth
//	const double additionalDistance = 0.65f;
//	RGBD::removeMapFeaturesWithoutDepth(mapFeatures,
//			currentSensorFrame.depthImage, additionalDistance,
//			frameIds, angles, depthImageScale);

	// Lets remove features that we cannot match due to max distance of Kinect/Asus
	RGBD::removeFarMapFeatures(mapFeatures, 5.0, frameIds, angles);

    //if (verbose > 0)
        //showMapFeatures(currentSensorFrame.rgbImage, mapFeatures, 0, "Map Features after");

	// Check some asserts
//				assert(("PUTSLAM: mapFeatures, frameIdsand angles", mapFeatures.size()
//								== frameIds.size()));
//				assert(("PUTSLAM: mapFeatures, frameIdsand angles", mapFeatures.size()
//								== angles.size()));

	return mapFeatures;
}

// Processing
void PUTSLAM::startProcessing() {
    getFrameEvent.lock();
	readingSomeParameters();
	initialization();

	int frameCounter = 0;
	auto startMainLoop = std::chrono::system_clock::now();
	SensorFrame lastSensorFrame;

	// Main loop
	while (true) {
		std::cout << std::endl << "----- Frame counter : " << frameCounter << "-----" << std::endl << std::flush;

//		// if loop was closed -> wait 10 seconds
//		if (map->getAndResetLoopClosureSuccesful())
//			usleep(10000000);


		// Get the frame to processing
		bool middleOfSequence = grabber->grab();
		if (!middleOfSequence)
			break;


        SensorFrame currentSensorFrame = grabber->getSensorFrame();

		if (drawImages) {
			cv::imshow("PUTSLAM RGB frame", currentSensorFrame.rgbImage);
			cv::imshow("PUTSLAM Depth frame", currentSensorFrame.depthImage);
		}

		int cameraPoseId = 0;
		bool addFeatureToMap = false;

		// Variable to store map features
		std::vector<MapFeature> mapFeatures;

//		std::cout << currentSensorFrame.depthImage.row(320) << std::endl;
//		std::cin >>addFeatureToMap;

		// The beginning of the sequence
		if (frameCounter == 0) {
			processFirstFrame(currentSensorFrame, cameraPoseId);



			addFeatureToMap = true;
		}
		// The next pose in the sequence
		else {


			// Running VO - matching or tracking depending on parameters
			// TODO:
			// - if no motion than skip frame
			std::vector<cv::DMatch> inlierMatches;
			Eigen::Matrix4f poseIncrement = runVO(currentSensorFrame, inlierMatches);

			double translationVO = sqrt(pow(poseIncrement(0,3),2) + pow(poseIncrement(1,3),2) + pow(poseIncrement(2,3),2));
			if (translationVO > 0.1)
				poseIncrement = Eigen::Matrix4f::Identity();


			VOPoseEstimate = VOPoseEstimate * poseIncrement;

			if (!onlyVO) {

				Stopwatch<> mapTime;
				mapTime.start();

				// Prediction from map
//				std::cout<<"poseIncrementVO: " << poseIncrement << std::endl;
//				std::cout<<"poseIncrementMap: " << getPoseIncrementFromMap(frameCounter) << std::endl << std::endl;
//				poseIncrement = getPoseIncrementFromMap(frameCounter);

				addPoseToMap(currentSensorFrame, poseIncrement, cameraPoseId);

				// Getting the pose
				Mat34 cameraPose = getMapPoseEstimate();

				// Get and filter the visible features
				// Find the ids of frames for which feature observations have the most similar angle
				std::vector<int> frameIds;
                std::vector<double> angles;
				mapFeatures = getAndFilterFeaturesFromMap(currentSensorFrame, cameraPose, frameIds, angles);

				// Save size of map features
				visibleMapFeaturesLog.push_back(mapFeatures.size());

				if (verbose > 0)
					std::cout << "Returned visible map feature size: "
							<< mapFeatures.size() << std::endl;

				// Show map features
				if (matcher->matcherParameters.verbose > 0)
					showMapFeatures(currentSensorFrame.rgbImage, mapFeatures, 1);

				// Perform RANSAC matching and return measurements for found inliers in map compatible format
				// Remember! The match returns the list of inlier features from current pose!
				std::vector<MapFeature> measurementList;
				Eigen::Matrix4f mapEstimatedTransformation;

				// Map matching based only on descriptors
				double mapMatchingInlierRatio = 0.0f;

				Stopwatch<> tmp;
				tmp.start();

				// Map matching based on descriptors, but in a sphere around feature with set radius
				bool newDetection = false;
				int tryCounter = 1;
				mapMatchingInlierRatio = matcher->Matcher::matchXYZ(mapFeatures,
						cameraPoseId, measurementList,
						mapEstimatedTransformation, newDetection, frameIds);
				while (mapMatchingInlierRatio < 0.1 && tryCounter < 10)
				{
					tryCounter++;
					mapMatchingInlierRatio = matcher->Matcher::matchXYZ(mapFeatures,
											cameraPoseId, measurementList,
											mapEstimatedTransformation, newDetection, frameIds, tryCounter);
					std::cout<<"Repeated matching: " << mapMatchingInlierRatio << std::endl;
				}


				tmp.stop();
				timeMeasurement.mapMatchingTimes.push_back((long int)tmp.elapsed());

				MapMatchingRansacInlierRatioLog.push_back(
						mapMatchingInlierRatio);

				if (verbose > 0)
					std::cout << "Measurement to features in graph size : "
							<< measurementList.size() << std::endl;


				tmp.start();

				// Add pose-pose constrain
				if ((int) measurementList.size() < maxMeasurementsToAddPoseToPoseEdge)
				{
					Mat34 cameraPoseIncrement = Mat34(
							poseIncrement.cast<double>());
					map->addMeasurement(cameraPoseId - 1, cameraPoseId,
							cameraPoseIncrement);
				}


				// Add pose-feature constrain
				measurementToMapSizeLog.push_back((int)measurementList.size());
				if ((int)measurementList.size()
						> minMeasurementsToAddPoseToFeatureEdge) {
					if (map->useUncertainty()) {
						matcher->computeNormals(currentSensorFrame.depthImage,
								measurementList,
								currentSensorFrame.depthImageScale);
						matcher->computeRGBGradients(
								currentSensorFrame.rgbImage,
								currentSensorFrame.depthImage, measurementList,
								currentSensorFrame.depthImageScale);
					}
					map->addMeasurements(measurementList);
				}


				tmp.stop();
				timeMeasurement.mapAddMeasurementTimes.push_back((long int)tmp.elapsed());

				// Insufficient number of features -> time to add some features
				if ((int)mapFeatures.size() < addFeaturesWhenMapSizeLessThan
						|| ((int) measurementList.size()
								< addFeaturesWhenMeasurementSizeLessThan
								&& (int)mapFeatures.size()
										< addNoFeaturesWhenMapSizeGreaterThan)) {
					addFeatureToMap = true;
				}

				mapTime.stop();
				timeMeasurement.mapTimes.push_back((long int) mapTime.elapsed());
			}
		}

		// Should we add some features to the map?
		if (addFeatureToMap && !onlyVO) {
			if (verbose > 0)
				std::cout << "Adding features to map " << std::endl;

			// Getting observed features
			Matcher::featureSet features = matcher->getFeatures();

			// Convert to mapFeatures format
			std::vector<RGBDFeature> mapFeaturesToAdd;
			int addedCounter = 0;

			// Lets process possible features to add
			addedCounter = chooseFeaturesToAddToMap(features, addedCounter,
					maxOnceFeatureAdd, mapFeatures,
					(float)minEuclideanDistanceOfFeatures, (float)minImageDistanceOfFeatures,
					cameraPoseId, mapFeaturesToAdd);
			if (map->useUncertainty()) {
				matcher->computeNormals(currentSensorFrame.depthImage,
						mapFeaturesToAdd, currentSensorFrame.depthImageScale);
				matcher->computeRGBGradients(currentSensorFrame.rgbImage,
						currentSensorFrame.depthImage, mapFeaturesToAdd,
						currentSensorFrame.depthImageScale);
			}

			// Finally, adding to map
			map->addFeatures(mapFeaturesToAdd, cameraPoseId);

			if (verbose > 0)
				std::cout << "map->addFeatures -> added " << addedCounter
						<< " features" << std::endl;

			addFeatureToMap = false;
		}

#ifdef BUILD_WITH_ROS
		//////////////////////////////////////////////////////////////////////////////////////ROS
		if(cameraPoseId%60 == 1 && workWithROS){
			current_time = ros::Time::now();
			publishPoseROS(cameraPoseId);
			publishPointCloudROS(cameraPoseId, currentSensorFrame);
			std::cout<<"\n";
		}
#endif
		
		// Saving features for Dominik
		//		Matcher::featureSet features = matcher->getFeatures();
		//		saveFeaturesToFile(features, currentSensorFrame.timestamp);

		// Save trajectory
		saveTrajectoryFreiburgFormat(VOPoseEstimate, trajectoryFreiburgStream,
				currentSensorFrame.timestamp);

		mapSize.push_back(map->getNumberOfFeatures());

		frameCounter++;


		lastSensorFrame = currentSensorFrame;


        RGBDimg=currentSensorFrame.rgbImage;
        depthImgimg=currentSensorFrame.depthImage;
        getFrameEvent.unlock();
	}
	auto elapsed = std::chrono::duration_cast < std::chrono::milliseconds
			> (std::chrono::system_clock::now() - startMainLoop);
	saveFPS(double(frameCounter) / ((double)elapsed.count() / 1000.0));

	saveStatistics();

	std::cout << "Job finished! Good bye :)" << std::endl;
}

void PUTSLAM::removeMapFeaturesWithoutGoodObservationAngle(
		std::vector<MapFeature> &mapFeatures, std::vector<int> &frameIds,
        std::vector<double> &angles) {
	auto mapFeaturesIter = mapFeatures.begin();
	auto frameIdsIter = frameIds.begin();
	auto anglesIter = angles.begin();

	for (; mapFeaturesIter != mapFeatures.end();) {
		if (*frameIdsIter == -1) {
			mapFeaturesIter = mapFeatures.erase(mapFeaturesIter);
			frameIdsIter = frameIds.erase(frameIdsIter);
			anglesIter = angles.erase(anglesIter);
		} else {
			++mapFeaturesIter;
			++frameIdsIter;
			++anglesIter;
		}
	}
}

// At the end

void PUTSLAM::saveStatistics() {
std::cout << "save2file\n";
	//map->save2file("createdMapFile.map", "preOptimizedGraphFile.g2o");

	// Wait for management thread to finish
	if (mapManagmentThreadVersion == MAPTHREAD_ON)
		map->finishManagementThr();  // Wait for optimization thread to finish

    // thread for geometric loop closure
	if (loopClosureThreadVersion == LCTHREAD_ON)
        map->finishLoopClosureThr();

	// We optimize only at the end if that version is chosen
	if (optimizationThreadVersion == OPTTHREAD_ATEND)
        map->startOptimizationThread(1, 1);

	// Wait for optimization thread to finish
	if (optimizationThreadVersion != OPTTHREAD_OFF)
		map->finishOptimization("graph_trajectory.res",
				"optimizedGraphFile.g2o");

	///for inverse SLAM problem
	//    for (int i=0; i<traj.size();i++){
	//        VertexSE3 vert(i, traj[i], i);
	//        ((FeaturesMap*) map)->updatePose(vert, true);
	//    }
//	if (optimizationThreadVersion != OPTTHREAD_OFF)
//		map->exportOutput("graph_trajectory.res", "optimizedGraphFile.g2o");

	// Save times
	std::cout << "Saving times" << std::endl;
	timeMeasurement.saveToFile();

	// Save statistics
	std::cout << "Saving logs to file" << std::endl;
	saveLogs();

	// Close trajectory stream
	trajectoryFreiburgStream.close();
	trajectoryMotionModelStream.close();

	// Run statistics
	std::cout << "Evaluating trajectory" << std::endl;
	evaluateResults(((FileGrabber*) grabber)->parameters.basePath,
			((FileGrabber*) grabber)->parameters.datasetName);

	// Save map
	std::cout << "Saving to octomap" << std::endl;
	if (octomap > 0)
		createAndSaveOctomap(depthImageScale);
}

void PUTSLAM::saveTrajectoryFreiburgFormat(Eigen::Matrix4f transformation,
		std::ofstream & estTrajectory, double timestamp) {
	std::ostringstream ossTimestamp;
	ossTimestamp << std::setfill('0') << std::setprecision(17) << timestamp;
	// Saving estimate in Freiburg format
	Eigen::Quaternion<float> Q(transformation.block<3, 3>(0, 0));
	estTrajectory << ossTimestamp.str() << " " << transformation(0, 3) << " "
			<< transformation(1, 3) << " " << transformation(2, 3) << " "
			<< Q.coeffs().x() << " " << Q.coeffs().y() << " " << Q.coeffs().z()
            << " " << Q.coeffs().w() << std::endl;
}

void PUTSLAM::saveFeaturesToFile(Matcher::featureSet features,
		double timestamp) {

	int whatever = system("mkdir featuresDir");

	std::ostringstream fileName;
	fileName << std::setfill('0') << std::setprecision(17) << timestamp;
	std::ofstream file("featuresDir/" + fileName.str() + ".features");

	for (unsigned long int i = 0; i < features.feature3D.size(); i++) {
		file << features.undistortedFeature2D[i].x << " "
				<< features.undistortedFeature2D[i].y << " "
				<< features.feature3D[i](0) << " " << features.feature3D[i](1)
				<< " " << features.feature3D[i](2) << std::endl;
	}
	file.close();
}

void PUTSLAM::saveFeaturesToFile(Matcher::featureSet features,
		std::vector<cv::DMatch> inlierMatches, double timestamp) {

	int whatever = system("mkdir featuresDir");

	std::ostringstream fileName;
	fileName << std::setfill('0') << std::setprecision(17) << timestamp;
	std::ofstream file("featuresDir/" + fileName.str() + ".features",
			std::ofstream::out | std::ofstream::app);

	for (unsigned long int i = 0; i < inlierMatches.size(); i++) {
		int id = inlierMatches[i].trainIdx;
		file << features.undistortedFeature2D[id].x << " "
				<< features.undistortedFeature2D[id].y << " "
				<< features.feature3D[id](0) << " " << features.feature3D[id](1)
				<< " " << features.feature3D[id](2) << std::endl;
	}
	file.close();
}

void PUTSLAM::saveFPS(double fps) {
    std::ofstream fileFPS;
	fileFPS.open("fps.res");
	fileFPS << fps;
	fileFPS.close();
}

void PUTSLAM::saveLogs() {
    std::ofstream statisticsLogStream("statistics.py");

    statisticsLogStream << "import matplotlib.pyplot as plt" << std::endl;
    statisticsLogStream << "import numpy as np" << std::endl;

	statisticsLogStream << "plt.ioff()" << std::endl;

	// VORansacInlierRatioLog
	statisticsLogStream << "VORansacInlierRatioLog = np.array([";
	for (unsigned long int a = 0; a < VORansacInlierRatioLog.size(); a++) {
		statisticsLogStream << VORansacInlierRatioLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
	statisticsLogStream
			<< "plt.plot(VORansacInlierRatioLog, label='-1 means no matches before RANSAC')"
            << std::endl;
	statisticsLogStream
            << "fig.suptitle('RANSAC inlier ratio in VO', fontsize=20)" << std::endl;
    statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
    statisticsLogStream << "plt.ylabel('Inlier ratio', fontsize=16)" << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
    statisticsLogStream << "plt.savefig('VORansacInlierRatio.png')" << std::endl;

	// VOFeaturesSizeLog
	statisticsLogStream << "VOFeaturesSizeLog = np.array([";
	for (unsigned long int a = 0; a < VOFeaturesSizeLog.size(); a++) {
		statisticsLogStream << VOFeaturesSizeLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
	statisticsLogStream
			<< "plt.plot(VOFeaturesSizeLog)"
            << std::endl;
	statisticsLogStream
            << "fig.suptitle('Number of features used in VO', fontsize=20)" << std::endl;
    statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
    statisticsLogStream << "plt.ylabel('Number of features', fontsize=16)" << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
    statisticsLogStream << "plt.savefig('VOFeaturesSize.png')" << std::endl;


	// mapSize
	statisticsLogStream << "mapSize = np.array([";
		for (unsigned long int a = 0; a < mapSize.size(); a++) {
			statisticsLogStream << mapSize[a] << ", ";
		}
		statisticsLogStream << "]);" << std::endl;

        statisticsLogStream << "fig = plt.figure()" << std::endl;
		statisticsLogStream
				<< "plt.plot(mapSize)"
                << std::endl;
		statisticsLogStream
                << "fig.suptitle('Number of features in map', fontsize=20)" << std::endl;
        statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
        statisticsLogStream << "plt.ylabel('Features counter', fontsize=16)" << std::endl;
        statisticsLogStream << "plt.legend() " << std::endl;
        statisticsLogStream << "plt.savefig('mapSize.png')" << std::endl;


	// MapMatchingRansacInlierRatioLog
	statisticsLogStream << "MapMatchingRansacInlierRatioLog = np.array([";
	for (unsigned long int a = 0; a < MapMatchingRansacInlierRatioLog.size(); a++) {
		statisticsLogStream << MapMatchingRansacInlierRatioLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
	statisticsLogStream
			<< "plt.plot(MapMatchingRansacInlierRatioLog, label='-1 means no matches before RANSAC')"
            << std::endl;
	statisticsLogStream
			<< "fig.suptitle('RANSAC inlier ratio in Map Matching', fontsize=20)"
            << std::endl;
    statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
    statisticsLogStream << "plt.ylabel('Inlier ratio', fontsize=16)" << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
	statisticsLogStream << "plt.savefig('MapMatchingRansacInlierRatioLog.png')"
            << std::endl;

	// Measurement to map size
	statisticsLogStream << "mapMeasurementSize = np.array([";
	for (unsigned long int a = 0; a < measurementToMapSizeLog.size(); a++) {
		statisticsLogStream << measurementToMapSizeLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
	statisticsLogStream
			<< "plt.plot(mapMeasurementSize, label='-1 means no matches before RANSAC')"
            << std::endl;
	statisticsLogStream
			<< "fig.suptitle('Measurement number to features in map', fontsize=20)"
            << std::endl;
    statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
	statisticsLogStream << "plt.ylabel('Measurement number', fontsize=16)"
            << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
    statisticsLogStream << "plt.savefig('mapMatchinggSize.png')" << std::endl;

	// Visible map features
	statisticsLogStream << "visibleMapFeatures = np.array([";
	for (unsigned long int a = 0; a < visibleMapFeaturesLog.size(); a++) {
		statisticsLogStream << visibleMapFeaturesLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
	statisticsLogStream
			<< "plt.plot(visibleMapFeatures)"
            << std::endl;
	statisticsLogStream
			<< "fig.suptitle('Number of visible features from map', fontsize=20)"
            << std::endl;
    statisticsLogStream << "plt.xlabel('Frame counter', fontsize=18)" << std::endl;
	statisticsLogStream << "plt.ylabel('Feature number', fontsize=16)"
            << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
    statisticsLogStream << "plt.savefig('visibleMapFeatures.png')" << std::endl;


	// LC matches
    std::vector<double> lcMatchingRatiosLog = map->getLoopClosureMatchingRatiosLog();
	statisticsLogStream << "lcMatchingRatiosLog = np.array([";
	for (unsigned long int a = 0; a < lcMatchingRatiosLog.size(); a++) {
		statisticsLogStream << lcMatchingRatiosLog[a] << ", ";
	}
	statisticsLogStream << "]);" << std::endl;

    statisticsLogStream << "fig = plt.figure()" << std::endl;
    statisticsLogStream << "plt.plot(lcMatchingRatiosLog)" << std::endl;
	statisticsLogStream << "fig.suptitle('Matching ratio in LC', fontsize=20)"
            << std::endl;
	statisticsLogStream << "plt.xlabel('Compared pair no.', fontsize=18)"
            << std::endl;
    statisticsLogStream << "plt.ylabel('Ratio', fontsize=16)" << std::endl;
    statisticsLogStream << "plt.legend() " << std::endl;
    statisticsLogStream << "plt.savefig('LCMatchingRatio.png')" << std::endl;

	statisticsLogStream.close();

    std::vector<LoopClosure::LCMatch> lcAnalyzedPairsLog = map->getLoopClosureAnalyzedPairsLog();
	std::ofstream lcAnalyzedPairsStream("LCAnalyzedPairs.log");
	lcAnalyzedPairsStream << "id1 id2 probability matchingRatio" << std::endl;
	for ( auto &p : lcAnalyzedPairsLog) {
		lcAnalyzedPairsStream << p.posesIds.first << " " << p.posesIds.second << " " << p.probability << " " << p.matchingRatio << std::endl;
	}
	lcAnalyzedPairsStream.close();

}

void PUTSLAM::evaluateResults(std::string basePath, std::string datasetName) {

	std::string fullPath = basePath + "/" + datasetName + "/";

//	std::string evalATEVO =
//			"python2 ../../scripts/evaluate_ate.py " + fullPath
//					+ "groundtruth.txt VO_trajectory.res --verbose --scale 1 --save_associations ate_association.res --plot VOAte.png > VOAte.res";
//	std::string evalATEMap =
//			"python2 ../../scripts/evaluate_ate.py " + fullPath
//					+ "groundtruth.txt graph_trajectory.res --verbose --scale 1 --save_associations ate_association.res --plot g2oAte.png > g2oAte.res";
//	std::string evalRPEVO =
//			"python2 ../../scripts/evaluate_rpe.py " + fullPath
//					+ "groundtruth.txt VO_trajectory.res --verbose --delta_unit 'f' --fixed_delta --plot VORpe.png > VORpe.res";
//	std::string evalRPEMap =
//			"python2 ../../scripts/evaluate_rpe.py " + fullPath
//					+ "groundtruth.txt graph_trajectory.res --verbose --delta_unit 'f' --fixed_delta --plot g2oRpe.png > g2oRpe.res";
//	try {
//		int tmp = std::system(evalATEVO.c_str());
//		tmp = std::system(evalATEMap.c_str());
//		tmp = std::system(evalRPEVO.c_str());
//		tmp = std::system(evalRPEMap.c_str());
//	} catch (std::system_error& error) {
//		std::cout << "Error: " << error.code() << " - " << error.what() << '\n';
//	}

    std::ofstream datasetNameStream("DatasetName");
	datasetNameStream << fullPath;
	datasetNameStream.close();

}

void PUTSLAM::showMapFeatures(cv::Mat rgbImage,
        std::vector<MapFeature> mapFeatures, int wait, std::string windowName) {
	std::vector<cv::KeyPoint> mapFeatures2D(mapFeatures.size());
	std::transform(mapFeatures.begin(), mapFeatures.end(),
			mapFeatures2D.begin(),
			[](const MapFeature& m) {return cv::KeyPoint((float)m.u,(float)m.v, 3);});

	cv::Mat img2draw;
	cv::drawKeypoints(rgbImage, mapFeatures2D, img2draw, cv::Scalar(0, 255, 0));

	cv::imshow(windowName.c_str(), img2draw);
	if ( wait )
		cv::waitKey(10000);
	else
		cv::waitKey(1);
}

#ifdef BUILD_WITH_ROS
//////////////////////////////////////////////////////////////////////////////////////////////ROS
void PUTSLAM::setWorkWithROS(){
	workWithROS = true;
}

void PUTSLAM::initROSpublishers(){
	cameraOdometryPublisher = nh.advertise<nav_msgs::Path>("camera_path", 50);
	cameraPointCloudPublisher = nh.advertise<sensor_msgs::PointCloud>("camera_PointCloud", 50);
}

void PUTSLAM::publishPoseROS(int cameraPoseId){
	geometry_msgs::PoseStamped pyk;
	nav_msgs::Path CameraPath;
	
	for (int i = 1; i < cameraPoseId + 1; i++){
		Mat34 lastPose = map->getSensorPose(i);
		//tf::poseEigenToMsg(lastPose, pyk); to nie działa dla tego to na dole jest kopią kodu który wykonuje ta funkcja
		//Eigen to geometry_msgs
		pyk.pose.position.x = lastPose.translation()[0];
		pyk.pose.position.y = lastPose.translation()[1];
		pyk.pose.position.z = lastPose.translation()[2];
		Eigen::Quaterniond q = (Eigen::Quaterniond)lastPose.linear();
		pyk.pose.orientation.x = q.x();
		pyk.pose.orientation.y = q.y();
		pyk.pose.orientation.z = q.z();
		pyk.pose.orientation.w = q.w();
		if (pyk.pose.orientation.w < 0) {
		  pyk.pose.orientation.x *= -1;
		  pyk.pose.orientation.y *= -1;
		  pyk.pose.orientation.z *= -1;
		  pyk.pose.orientation.w *= -1;
		}
		pyk.header.stamp = current_time;
		pyk.header.frame_id = "camera_base";
		CameraPath.poses.push_back(pyk);
	}
	CameraPath.header.stamp = current_time;
	CameraPath.header.frame_id = "camera_base";
	cameraOdometryPublisher.publish(CameraPath);
}

void PUTSLAM::publishPointCloudROS(int cameraPoseId, const SensorFrame &currentSensorFrame){
	
	Mat34 lastPose = map->getSensorPose(cameraPoseId);
	Eigen::Matrix4f tmpPose = Eigen::Matrix4f(lastPose.matrix().cast<float>());
	// Creating color cloud
	std::vector<std::pair<Eigen::Vector3f, Eigen::Vector3i>> colorPointCloud =
			RGBD::imageToColorPointCloud(currentSensorFrame.rgbImage, currentSensorFrame.depthImage, matcher->matcherParameters.cameraMatrixMat, tmpPose, currentSensorFrame.depthImageScale);
	
	sensor_msgs::PointCloud pubPointCloud;
	pubPointCloud.header.frame_id = "camera_base";
	pubPointCloud.header.stamp = current_time;
	int size = colorPointCloud.size();
	//pubPointCloud.points.clear();
	//pubPointCloud.channels.clear();
	geometry_msgs::Point32 point;
	sensor_msgs::ChannelFloat32 colors[3];
	colors[0].name = "r";
	colors[1].name = "g";
	colors[2].name = "b";
	
	// We add every point
	for (int k = 0; k < size; k++) {
		point.x = (float) colorPointCloud[k].first.x();
		point.y = (float) colorPointCloud[k].first.y();
		point.z = (float) colorPointCloud[k].first.z();
		colors[0].values.push_back(((float)colorPointCloud[k].second.x())/256);
		colors[1].values.push_back(((float)colorPointCloud[k].second.y())/256);
		colors[2].values.push_back(((float)colorPointCloud[k].second.z())/256);
		pubPointCloud.points.push_back(point);
	}
	pubPointCloud.channels.push_back(colors[0]);
	pubPointCloud.channels.push_back(colors[1]);
	pubPointCloud.channels.push_back(colors[2]);
	cameraPointCloudPublisher.publish(pubPointCloud);	
}
#endif
