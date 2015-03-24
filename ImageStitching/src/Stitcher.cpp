/*
 * Stitcher.cpp
 *
 *  Created on: Feb 25, 2015
 *      Author: nvkhoi
 */

#include "Stitcher.h"

bool sortX(cv::Point a, cv::Point b) {
	return (a.x < b.x);
}

bool sortY(cv::Point a, cv::Point b) {
	return (a.y < b.y);
}

int Stitcher::registration(std::vector<cv::detail::CameraParams>& cameras) {

	int retVal = 1; //1 is normal, 0 is not enough, -1 is failed

	num_images = full_img.size();
	img.resize(num_images);
	images.resize(num_images);
	full_img_sizes.resize(num_images);

	std::vector<cv::detail::ImageFeatures> features(num_images);
	{
		double seam_scale = 1;
		cv::Ptr<cv::detail::FeaturesFinder> finder;
		if (registration_resol == 0.3)
			finder = new cv::detail::OrbFeaturesFinder(cv::Size(1, 1), 1500,
					1.0f, 5);
		else
			finder = new cv::detail::OrbFeaturesFinder(cv::Size(2, 2), 5000,
					1.0f, 5);

		if (registration_resol > 0)
			work_scale = std::min(1.0,
					sqrt(registration_resol * 1e6 / full_img[0].size().area()));
		else
			work_scale = 1;
		std::vector<cv::Rect> ROI;

		seam_scale = std::min(1.0,
				sqrt(seam_estimation_resol * 1e6 / full_img[0].size().area()));
		seam_work_aspect = seam_scale / work_scale;

#pragma omp parallel for
		for (int i = 0; i < num_images; ++i) {
			full_img_sizes[i] = full_img[i].size();
			if (registration_resol <= 0)
				img[i] = full_img[i];
			else
				cv::resize(full_img[i], img[i], cv::Size(), work_scale,
						work_scale);

			(*finder)(img[i], features[i]);
			features[i].img_idx = i;
			cv::resize(full_img[i], img[i], cv::Size(), seam_scale, seam_scale);
			images[i] = img[i].clone();
		}
		finder->collectGarbage();
	}

	std::vector<cv::detail::MatchesInfo> pairwise_matches;
	{
		cv::detail::BestOf2NearestMatcher matcher;
		if (matching_mask.rows * matching_mask.cols <= 1)
			matcher(features, pairwise_matches);
		else
			matcher(features, pairwise_matches, matching_mask);
		matcher.collectGarbage();

		// Leave only images we are sure are from the same panorama
		std::vector<int> indices = leaveBiggestComponent(features,
				pairwise_matches, confidence_threshold);
		std::vector<cv::Mat> img_subset;
		std::vector<cv::Size> full_img_sizes_subset;
		std::vector<cv::Mat> full_img_subset;
		for (size_t i = 0; i < indices.size(); ++i) {
			img_subset.push_back(images[indices[i]]);
			full_img_sizes_subset.push_back(full_img_sizes[indices[i]]);
			full_img_subset.push_back(full_img[indices[i]]);
		}
		images = img_subset;
		full_img_sizes = full_img_sizes_subset;
		full_img = full_img_subset;
		// Check if we still have enough images
		int tmp = static_cast<int>(images.size());
		{
			std::ostringstream os;
			os << tmp << "/" << num_images;
			std::string stmp = os.str();
			os << std::left << std::setw(10 - stmp.length()) << "";
			status = status + os.str();
		}
		if (tmp < num_images) {
			num_images = tmp;
			retVal = 0;
		}
		if (num_images < 2)
			return -1;
	}

	{
		cv::detail::HomographyBasedEstimator estimator;
		estimator(features, pairwise_matches, cameras);
#pragma omp parallel for
		for (size_t i = 0; i < cameras.size(); ++i) {
			cv::Mat R;
			cameras[i].R.convertTo(R, CV_32F);
			cameras[i].R = R;
		}
		cv::Ptr<cv::detail::BundleAdjusterBase> adjuster;
		adjuster = new cv::detail::BundleAdjusterRay();

		adjuster->setConfThresh(confidence_threshold);
		cv::Mat_<uchar> refine_mask = cv::Mat::zeros(3, 3, CV_8U);

		refine_mask(0, 0) = 1;
		refine_mask(0, 1) = 1;
		refine_mask(0, 2) = 1;
		refine_mask(1, 1) = 1;
		refine_mask(1, 2) = 1;

		adjuster->setRefinementMask(refine_mask);

		(*adjuster)(features, pairwise_matches, cameras);
		// Find median focal length
		std::vector<double> focals(cameras.size());
#pragma omp parallel for
		for (unsigned int i = 0; i < cameras.size(); i++)
			focals[i] = cameras[i].focal;
		sort(focals.begin(), focals.end());
		if (focals.size() % 2 == 1)
			warped_image_scale = static_cast<float>(focals[focals.size() / 2]);
		else
			warped_image_scale =
					static_cast<float>(focals[focals.size() / 2 - 1]
							+ focals[focals.size() / 2]) * 0.5f;

		std::vector<cv::Mat> rmats(cameras.size());
#pragma omp parallel for
		for (unsigned int i = 0; i < cameras.size(); i++)
			rmats[i] = cameras[i].R;
		waveCorrect(rmats, cv::detail::WAVE_CORRECT_HORIZ);
#pragma omp parallel for
		for (unsigned int i = 0; i < cameras.size(); i++)
			cameras[i].R = rmats[i];
	}
	return retVal;

}

cv::Mat Stitcher::compositing(std::vector<cv::detail::CameraParams>& cameras) {
	cv::Ptr<cv::WarperCreator> warper_creator;
	std::vector<cv::Ptr<cv::detail::RotationWarper> > warper(num_images);
	{
		// Warp images and their masks
		switch (warp_type) {
		case PLANE:
			warper_creator = new cv::PlaneWarper();
			break;
		case CYLINDRICAL:
			warper_creator = new cv::CylindricalWarper();
			break;
		case SPHERICAL:
			warper_creator = new cv::SphericalWarper();
			break;
		case FISHEYE:
			warper_creator = new cv::FisheyeWarper();
			break;
		case STEREOGRAPHIC:
			warper_creator = new cv::StereographicWarper();
			break;
		case COMPRESSEDPLANEA2B1:
			warper_creator = new cv::CompressedRectilinearWarper(2, 1);
			break;
		case COMPRESSEDPLANEA15B1:
			warper_creator = new cv::CompressedRectilinearWarper(1.5, 1);
			break;
		case COMPRESSEDPORTRAITA2B1:
			warper_creator = new cv::CompressedRectilinearPortraitWarper(2, 1);
			break;
		case COMPRESSEDPORTRAITA15B1:
			warper_creator = new cv::CompressedRectilinearPortraitWarper(1.5,
					1);
			break;
		case PANINIA2B1:
			warper_creator = new cv::PaniniWarper(2, 1);
			break;
		case PANINIA15B1:
			warper_creator = new cv::PaniniWarper(1.5, 1);
			break;
		case PANINIPORTRAITA2B1:
			warper_creator = new cv::PaniniPortraitWarper(2, 1);
			break;
		case PANINIPORTRAITA15B1:
			warper_creator = new cv::PaniniPortraitWarper(1.5, 1);
			break;
		case MERCATOR:
			warper_creator = new cv::MercatorWarper();
			break;
		case TRANSVERSEMERCATOR:
			warper_creator = new cv::TransverseMercatorWarper();
			break;
		}
#pragma omp parallel for
		for (int i = 0; i < num_images; i++)
			warper[i] = warper_creator->create(
					static_cast<float>(warped_image_scale * seam_work_aspect));
	}

	std::vector<cv::Point> corners(num_images);
	std::vector<cv::Mat> masks_warped(num_images);
	cv::Ptr<cv::detail::ExposureCompensator> compensator;
	std::vector<cv::Size> sizes(num_images);
	{

		std::vector<cv::Mat> images_warped(num_images);
		std::vector<cv::Mat> masks(num_images);
		// Prepare images masks
#pragma omp parallel for
		for (int i = 0; i < num_images; ++i) {
			masks[i].create(images[i].size(), CV_8U);
			masks[i].setTo(cv::Scalar::all(255));
		}
		// Warp images and their masks
		std::vector<cv::Mat> images_warped_f(num_images);
#pragma omp parallel for
		for (int i = 0; i < num_images; ++i) {
			cv::Mat_<float> K;
			cameras[i].K().convertTo(K, CV_32F);
			float swa = (float) seam_work_aspect;
			K(0, 0) *= swa;
			K(0, 2) *= swa;
			K(1, 1) *= swa;
			K(1, 2) *= swa;

			corners[i] = warper[i]->warp(images[i], K, cameras[i].R,
					cv::INTER_LINEAR, cv::BORDER_REFLECT, images_warped[i]);
			sizes[i] = images_warped[i].size();

			warper[i]->warp(masks[i], K, cameras[i].R, cv::INTER_NEAREST,
					cv::BORDER_CONSTANT, masks_warped[i]);
			images_warped[i].convertTo(images_warped_f[i], CV_32F);
		}
		compensator = cv::detail::ExposureCompensator::createDefault(
				expos_comp_type);
		compensator->feed(corners, images_warped, masks_warped);
		cv::Ptr<cv::detail::SeamFinder> seam_finder;
		switch (seam_find_type) {
		case NO:
			seam_finder = new cv::detail::NoSeamFinder();
			break;
		case VORONOI:
			seam_finder = new cv::detail::VoronoiSeamFinder();
			break;
		case GC_COLOR:
			seam_finder = new cv::detail::GraphCutSeamFinder(
					cv::detail::GraphCutSeamFinderBase::COST_COLOR);
			break;
		case GC_COLORGRAD:
			seam_finder = new cv::detail::GraphCutSeamFinder(
					cv::detail::GraphCutSeamFinderBase::COST_COLOR_GRAD);
			break;
		case DP_COLOR:
			seam_finder = new cv::detail::DpSeamFinder(
					cv::detail::DpSeamFinder::COLOR);
			break;
		case DP_COLORGRAD:
			seam_finder = new cv::detail::DpSeamFinder(
					cv::detail::DpSeamFinder::COLOR_GRAD);
			break;
		}
		seam_finder->find(images_warped_f, corners, masks_warped);
		// Release unused memory
		images.clear();
		images_warped.clear();
		images_warped_f.clear();
		masks.clear();
	}

	cv::Mat result;
	{
		cv::Ptr<cv::detail::Blender> blender;
		double compose_scale = 1;
		double compose_work_aspect = 1;
		if (compositing_resol > 0)
			compose_scale = std::min(1.0,
					sqrt(compositing_resol * 1e6 / full_img[0].size().area()));

		// Compute relative scales
		compose_work_aspect = compose_scale / work_scale;

		// Update warped image scale
		warped_image_scale *= static_cast<float>(compose_work_aspect);
#pragma omp parallel for
		for (int i = 0; i < num_images; i++)
			warper[i] = warper_creator->create(warped_image_scale);

		// Update corners and sizes
#pragma omp parallel for
		for (int i = 0; i < num_images; ++i) {
			// Update intrinsics
			cameras[i].focal *= compose_work_aspect;
			cameras[i].ppx *= compose_work_aspect;
			cameras[i].ppy *= compose_work_aspect;

			// Update corner and size
			cv::Size sz = full_img_sizes[i];
			if (std::abs(compose_scale - 1) > 1e-1) {
				sz.width = cvRound(full_img_sizes[i].width * compose_scale);
				sz.height = cvRound(full_img_sizes[i].height * compose_scale);
			}

			cv::Mat K;
			cameras[i].K().convertTo(K, CV_32F);
			cv::Rect roi = warper[i]->warpRoi(sz, K, cameras[i].R);
			corners[i] = roi.tl();
			sizes[i] = roi.size();
		}
		blender = cv::detail::Blender::createDefault(blend_type, false);
		cv::Size dst_sz = cv::detail::resultRoi(corners, sizes).size();
		float blend_width = sqrt(static_cast<float>(dst_sz.area()))
				* blend_strength / 100.f;
		if (blend_width < 1.f)
			blender = cv::detail::Blender::createDefault(
					cv::detail::Blender::NO, false);
		else if (blend_type == cv::detail::Blender::MULTI_BAND) {
			cv::detail::MultiBandBlender* mb =
					dynamic_cast<cv::detail::MultiBandBlender*>(static_cast<cv::detail::Blender*>(blender));
			mb->setNumBands(
					static_cast<int>(ceil(log(blend_width) / log(2.)) - 1.));

		} else if (blend_type == cv::detail::Blender::FEATHER) {
			cv::detail::FeatherBlender* fb =
					dynamic_cast<cv::detail::FeatherBlender*>(static_cast<cv::detail::Blender*>(blender));
			fb->setSharpness(1.f / blend_width);

		}
		blender->prepare(corners, sizes);
#pragma omp parallel for
		for (int img_idx = 0; img_idx < num_images; ++img_idx) {
			cv::Mat mask_warped, img_warped_s, dilated_mask, seam_mask, mask,
					img_warped;
			// Read image and resize it if necessary
			if (abs(compose_scale - 1) > 1e-1)
				resize(full_img[img_idx], img[img_idx], cv::Size(),
						compose_scale, compose_scale);
			else
				img[img_idx] = full_img[img_idx];
			cv::Size img_size = img[img_idx].size();

			cv::Mat K;
			cameras[img_idx].K().convertTo(K, CV_32F);

			// Warp the current image
			warper[img_idx]->warp(img[img_idx], K, cameras[img_idx].R,
					cv::INTER_LINEAR, cv::BORDER_REFLECT, img_warped);

			// Warp the current image mask
			mask.create(img_size, CV_8U);
			mask.setTo(cv::Scalar::all(255));
			warper[img_idx]->warp(mask, K, cameras[img_idx].R,
					cv::INTER_NEAREST, cv::BORDER_CONSTANT, mask_warped);

			// Compensate exposure
			compensator->apply(img_idx, corners[img_idx], img_warped,
					mask_warped);

			img_warped.convertTo(img_warped_s, CV_16S);

			dilate(masks_warped[img_idx], dilated_mask, cv::Mat());
			resize(dilated_mask, seam_mask, mask_warped.size());
			mask_warped = seam_mask & mask_warped;

			// Blend the current image
			blender->feed(img_warped_s, mask_warped, corners[img_idx]);
		}
		full_img.clear();
		img.clear();
		cv::Mat result_mask;
		blender->blend(result, result_mask);
	}

	return result;
}
bool Stitcher::checkInteriorExterior(const cv::Mat& mask,
		const cv::Rect& interiorBB, int& top, int& bottom, int& left,
		int& right) {
// return true if the rectangle is fine as it is!

	cv::Mat sub = mask(interiorBB);
// count how many exterior pixels are at the
	unsigned int cTop = 0; // top row
	unsigned int cBottom = 0; // bottom row
	unsigned int cLeft = 0; // left column
	unsigned int cRight = 0; // right column
// and choose that side for reduction where more exterior pixels occurring (that's the heuristic)

	for (int x = 0; x < sub.cols; ++x) {
		// if there is an exterior part in the interior we have to move the top side of the rect a bit to the bottom
		if (sub.at<unsigned char>(0, x) == 0)
			++cTop;
		if (sub.at<unsigned char>(sub.rows - 1, x) == 0)
			++cBottom;
	}

	for (int y = 0; y < sub.rows; ++y) {
		// if there is an exterior part in the interior
		if (sub.at<unsigned char>(y, 0) == 0)
			++cLeft;
		if (sub.at<unsigned char>(y, sub.cols - 1) == 0)
			++cRight;
	}

// that part is ugly and maybe not correct, didn't check whether all possible combinations are handled. Check that one please. The idea is to set `top = 1` iff it's better to reduce the rect at the top than anywhere else.
	if ((cTop + cBottom + cLeft + cRight == 0))
		return true;

	if (cTop >= cBottom) {
		if (cTop >= cLeft)
			if (cTop >= cRight)
				top = 1;
	} else if (cBottom >= cLeft)
		if (cBottom >= cRight)
			bottom = 1;

	if (cLeft >= cRight) {
		if (cLeft >= cBottom)
			if (cLeft >= cTop)
				left = 1;
	} else if (cRight >= cTop)
		if (cRight >= cBottom)
			right = 1;
	return false;
}

cv::Mat Stitcher::crop(const cv::Mat& orig) {
	if (orig.rows * orig.cols == 1)
		return orig;
	cv::Mat gray, tmp;
	orig.convertTo(tmp, CV_32F);
	cv::cvtColor(tmp, gray, CV_RGB2GRAY);

	// extract all the black background (and some interior parts maybe)
	cv::Mat mask = gray > 0;

	// now extract the outer contour
	std::vector<std::vector<cv::Point> > contours;
	std::vector<cv::Vec4i> hierarchy;

	cv::findContours(mask, contours, hierarchy, CV_RETR_EXTERNAL,
			CV_CHAIN_APPROX_NONE, cv::Point(0, 0));

	cv::Mat contourImage = cv::Mat::zeros(orig.size(), CV_8UC3);

	//find contour with max elements
	// remark: in theory there should be only one single outer contour surrounded by black regions!!

	unsigned int maxSize = 0;
	unsigned int id = 0;
	for (unsigned int i = 0; i < contours.size(); ++i) {
		if (contours.at(i).size() > maxSize) {
			maxSize = contours.at(i).size();
			id = i;
		}
	}

	/// Draw filled contour to obtain a mask with interior parts
	cv::Mat contourMask = cv::Mat::zeros(orig.size(), CV_8UC1);
	cv::drawContours(contourMask, contours, id, cv::Scalar(255), -1, 8,
			hierarchy, 0, cv::Point());
	// sort contour in x/y directions to easily find min/max and next
	std::vector<cv::Point> cSortedX = contours.at(id);
	std::sort(cSortedX.begin(), cSortedX.end(), sortX);

	std::vector<cv::Point> cSortedY = contours.at(id);
	std::sort(cSortedY.begin(), cSortedY.end(), sortY);

	unsigned int minXId = 0;
	unsigned int maxXId = cSortedX.size() - 1;

	unsigned int minYId = 0;
	unsigned int maxYId = cSortedY.size() - 1;

	cv::Rect interiorBB;

	while ((minXId < maxXId) && (minYId < maxYId)) {

		interiorBB = cv::Rect(cSortedX[minXId].x, cSortedY[minYId].y,
				cSortedX[maxXId].x - cSortedX[minXId].x,
				cSortedY[maxYId].y - cSortedY[minYId].y);

		// out-codes: if one of them is set, the rectangle size has to be reduced at that border
		int ocTop = 0;
		int ocBottom = 0;
		int ocLeft = 0;
		int ocRight = 0;

		if (checkInteriorExterior(contourMask, interiorBB, ocTop, ocBottom,
				ocLeft, ocRight)) {
			break;
		}

		// reduce rectangle at border if necessary
		if (ocLeft)
			++minXId;
		if (ocRight)
			--maxXId;
		if (ocTop)
			++minYId;
		if (ocBottom)
			--maxYId;

	}
	cv::Mat maskedImage;
	orig(interiorBB).copyTo(maskedImage);
	return maskedImage;
}

Stitcher::Stitcher() {
	// TODO Auto-generated constructor stub
	warped_image_scale = 1.0;
	num_images = 0;
	blend_type = cv::detail::Blender::MULTI_BAND;
	blend_strength = 5;
	seam_work_aspect = 1;
	work_scale = 1;
	registration_resol = 0.3;
	seam_estimation_resol = 0.08;
	confidence_threshold = 0.6;
	warp_type = CYLINDRICAL;
	seam_find_type = DP_COLORGRAD;
	expos_comp_type = cv::detail::ExposureCompensator::GAIN;
	compositing_resol = -1.0;
	status = "";
	matching_mask = cv::Mat(1, 1, CV_8U, cv::Scalar(0));
}

Stitcher::Stitcher(const int& mode) {
	warped_image_scale = 1.0;
	num_images = 0;
	blend_type = cv::detail::Blender::MULTI_BAND;
	blend_strength = 5;
	seam_work_aspect = 1;
	work_scale = 1;
	switch (mode) {
	case FAST: {
		registration_resol = 0.3;
		seam_estimation_resol = 0.08;
		confidence_threshold = 0.6;
		warp_type = CYLINDRICAL;
		seam_find_type = DP_COLORGRAD;
		expos_comp_type = cv::detail::ExposureCompensator::GAIN;
		compositing_resol = -1.0;
	}
		;
		break;
	case PREVIEW: {
		registration_resol = 0.3;
		seam_estimation_resol = 0.08;
		confidence_threshold = 0.6;
		warp_type = CYLINDRICAL;
		seam_find_type = DP_COLORGRAD;
		expos_comp_type = cv::detail::ExposureCompensator::GAIN;
		compositing_resol = 0.4;
	}
		;
		break;
	default: {
		registration_resol = 0.6;
		seam_estimation_resol = 0.1;
		confidence_threshold = 1.0;
		warp_type = SPHERICAL;
		seam_find_type = GC_COLOR;
		expos_comp_type = cv::detail::ExposureCompensator::GAIN_BLOCKS;
		compositing_resol = -1.0;
	}
	}
	status = "";
	matching_mask = cv::Mat(1, 1, CV_8U, cv::Scalar(0));
}

void Stitcher::set_matching_mask(const std::string& file_name,
		std::vector<std::pair<int, int> >& edge_list) {
	struct stat buf;
	if (stat(file_name.c_str(), &buf) != -1) {
		std::ifstream pairwise(file_name.c_str(), std::ifstream::in);
		while (true) {
			int i, j;
			pairwise >> i >> j;
			std::pair<int, int> t;
			t = {i,j};
			edge_list.push_back(t);
			if (pairwise.eof())
				break;
		}
		pairwise.close();
	}
}

int Stitcher::rotate_img(const std::string& img_path) {
	try {
		Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(img_path);

		assert(image.get() != 0);

		image->readMetadata();
		Exiv2::ExifData &exifData = image->exifData();
		if (exifData.empty()) throw Exiv2::Error(1, "No exif data found!");
		Exiv2::ExifData::const_iterator i = exifData.findKey(
				*(new Exiv2::ExifKey("Exif.Image.Orientation")));
		int angle = i->value().toLong();
		switch (angle) {
		case 6: {
#pragma omp parallel for
			for (int i = 0; i < num_images; i++) {
				cv::Mat tmp;
				cv::transpose(full_img[i], tmp);
				cv::flip(tmp, tmp, 1);
				full_img[i] = tmp.clone();
			}

		}
			break;
		case 3: {
#pragma omp parallel for
			for (int i = 0; i < num_images; i++)
				cv::flip(full_img[i], full_img[i], -1);

		}
			break;
		}
	} catch (Exiv2::AnyError& e) {
		std::cout << e.what() << '\n';
		return -1;
	}
	return 0;
}

void Stitcher::feed(const std::string& dir) {
	boost::filesystem::path dir_path(dir);
	try {
		if (boost::filesystem::exists(dir_path)
				&& boost::filesystem::is_directory(dir_path)) {
			std::vector<boost::filesystem::path> file_list;
			std::copy(boost::filesystem::directory_iterator(dir_path),
					boost::filesystem::directory_iterator(),
					std::back_inserter(file_list));
			std::sort(file_list.begin(), file_list.end());
			std::vector<std::pair<int, int> > edge_list;
			bool rotate_check = false;
			std::string sample_path;
			for (auto i : file_list) {
				if (boost::filesystem::is_regular_file(i)) {
					std::string file_name = i.string();
					std::string ext = file_name.substr(
							file_name.find_last_of(".") + 1);
					if (ext == "JPG" || ext == "jpg" || ext == "PNG"
							|| ext == "png") {
						full_img.push_back(cv::imread(file_name));
						if (!rotate_check) {
							sample_path = file_name;
							rotate_check = true;
						}
					}

					else if (ext == "TXT" || ext == "txt")
						set_matching_mask(file_name, edge_list);
				}
			}
			num_images = full_img.size();
			if (!edge_list.empty()) {
				matching_mask = cv::Mat(num_images, num_images, CV_8U,
						cv::Scalar(0));
				for (auto i : edge_list)
					matching_mask.at<char>(i.first, i.second) = 1;
			}
			rotate_img(sample_path);

		}
	} catch (const boost::filesystem::filesystem_error& ex) {
		return;
	}
}

void Stitcher::set_dst(const std::string &dst) {
	result_dst = dst;
}

std::string Stitcher::get_dst() {
	return result_dst;
}

enum Stitcher::ReturnCode Stitcher::stitching_process(cv::Mat& result) {
	enum ReturnCode retVal = OK;
	if (full_img.size() < 2)
		retVal = NEED_MORE;
	else {
		std::vector<cv::detail::CameraParams> cameras;
		int check = registration(cameras);
		if (check == -1)
			retVal = FAILED;
		else {
			if (check == 0)
				retVal = NOT_ENOUGH;
			result = compositing(cameras);
			//result = crop(result);
			if (result.rows * result.cols == 1)
				retVal = FAILED;
		}

	}
	std::ostringstream os;
	switch (retVal) {
	case NOT_ENOUGH: {
		os << std::left << std::setw(15) << "Not enough!";
		status = status + os.str();
	}
		break;
	case OK: {
		os << std::left << std::setw(15) << "Succeed!";
		status = status + os.str();
	}
		break;
	case NEED_MORE: {
		os << std::left << std::setw(15) << "Need more!";
		status = status + os.str();
	}
		break;
	case FAILED: {
		os << std::left << std::setw(15) << "Failed!";
		status = status + os.str();
	}
		break;
	}

	return retVal;
}

void Stitcher::stitch() {
	cv::Mat result;
	std::vector<cv::Mat> img_bak = full_img;
	enum ReturnCode code = stitching_process(result);
	std::string tmp = status;
	status = "";
	if (code != OK) {
		cv::Mat retry;
		full_img = img_bak;
		Stitcher(DEFAULT);
		stitching_process(retry);
		if ((result.cols * result.rows) < (retry.cols * retry.rows)) {
			result = retry.clone();
		} else
			status = tmp;
	}
	tmp = result_dst + ".jpg";
	cv::imwrite(tmp, result);
	double scale = double(1080) / result.rows;
	cv::Mat preview;
	cv::resize(result, preview, cv::Size(), scale, scale);
	result_dst = result_dst + "p.jpg";
	cv::imwrite(result_dst, preview);
}

std::string Stitcher::to_string() {
	return status;
}

Stitcher::~Stitcher() {
	// TODO Auto-generated destructor stub
	full_img.clear();
	img.clear();
	images.clear();
	full_img_sizes.clear();
}
