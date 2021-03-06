/*
 * main.cpp
 *
 *  Created on: Feb 25, 2015
 *      Author: nvkhoi
 */

#include <cstdio>
#include "Stitcher.h"

std::string uploadDir = "./uploads/", publicDir = "./public/";
std::string workingDir;

int main(int argc, char* argv[]) {
#if ON_LOGGER
	FILE *f_out = freopen("detail.txt", "a", stdout);
	if (f_out == NULL)
		freopen("/dev/tty", "a", stdout);
#endif
	cv::setBreakOnError(true);
	cv::setUseOptimized(true);
	if (argc < 2)
		return -1;
	long long start;
	for (int i = 1; i < argc; i++) {
#if ON_LOGGER
		printf("%s\n", argv[i]);
#endif
		workingDir = argv[i];
		Stitcher stitcher;
		std::string dst = publicDir + workingDir;
#if ON_LOGGER
		start = cv::getTickCount();
#endif
		stitcher.set_dst(dst);
#if ON_LOGGER
		printf("%lf\n",
				(double(cv::getTickCount()) - start) / cv::getTickFrequency());
#endif

		workingDir = uploadDir + workingDir + "/";

#if ON_LOGGER
		start = cv::getTickCount();
#endif
		stitcher.feed(workingDir);
#if ON_LOGGER
		printf("%lf\n",
				(double(cv::getTickCount()) - start) / cv::getTickFrequency());
#endif

#if ON_LOGGER
		start = cv::getTickCount();
#endif
		stitcher.stitch();
#if ON_LOGGER
		printf("%s\n", stitcher.get_status().c_str());
		printf("%lf\n",
				(double(cv::getTickCount()) - start) / cv::getTickFrequency());
#endif
	}

	return 0;
}
