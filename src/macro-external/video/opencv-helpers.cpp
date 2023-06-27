#include "opencv-helpers.hpp"
#include "log-helper.hpp"

#include <opencv2/core/ocl.hpp>
#include <opencv2/core/mat.hpp>

namespace advss {

PatternImageData CreatePatternData(const QImage &pattern)
{
	PatternImageData data{};
	if (pattern.isNull()) {
		return data;
	}

	data.rgbaPattern = QImageToMat(pattern);
	std::vector<cv::Mat1b> rgbaChannelsPattern;
	cv::split(data.rgbaPattern, rgbaChannelsPattern);
	std::vector<cv::Mat1b> rgbChanlesPattern(
		rgbaChannelsPattern.begin(), rgbaChannelsPattern.begin() + 3);
	cv::merge(rgbChanlesPattern, data.rgbPattern);
	cv::threshold(rgbaChannelsPattern[3], data.mask, 0, 255,
		      cv::THRESH_BINARY);
	return data;
}

static void invertPatternMatchResult(cv::UMat &umat)
{
	auto mat = umat.getMat(cv::ACCESS_RW);
	for (int r = 0; r < mat.rows; r++) {
		for (int c = 0; c < mat.cols; c++) {
			mat.at<float>(r, c) = 1.0 - mat.at<float>(r, c);
		}
	}
	umat = mat.getUMat(cv::ACCESS_RW);
}

void MatchPattern(QImage &img, const PatternImageData &patternData,
		  double threshold, cv::UMat &result, bool useAlphaAsMask,
		  cv::TemplateMatchModes matchMode)
{
	if (img.isNull() || patternData.rgbaPattern.empty()) {
		return;
	}
	if (img.height() < patternData.rgbaPattern.rows ||
	    img.width() < patternData.rgbaPattern.cols) {
		return;
	}

	auto input = QImageToMat(img);

	if (useAlphaAsMask) {
		// Remove alpha channel of input image as the alpha channel
		// information is used as a stencil for the pattern instead and
		// thus should not be used while matching the pattern as well
		//
		// Input format is Format_RGBA8888 so discard the 4th channel
		std::vector<cv::UMat> inputChannels;
		cv::split(input, inputChannels);
		std::vector<cv::UMat> rgbChanlesImage(
			inputChannels.begin(), inputChannels.begin() + 3);
		cv::UMat rgbInput;
		cv::merge(rgbChanlesImage, rgbInput);
		cv::matchTemplate(rgbInput, patternData.rgbPattern, result,
				  matchMode, patternData.mask);
	} else {
		cv::matchTemplate(input, patternData.rgbaPattern, result,
				  matchMode);
	}

	// A perfect match is represented as "0" for TM_SQDIFF_NORMED
	//
	// For TM_CCOEFF_NORMED and TM_CCORR_NORMED a perfect match is
	// represented as "1"
	if (matchMode == cv::TM_SQDIFF_NORMED) {
		invertPatternMatchResult(result);
	}
	cv::threshold(result, result, threshold, 0.0, cv::THRESH_TOZERO);
}

void MatchPattern(QImage &img, QImage &pattern, double threshold,
		  cv::UMat &result, bool useAlphaAsMask,
		  cv::TemplateMatchModes matchColor)
{
	auto data = CreatePatternData(pattern);
	MatchPattern(img, data, threshold, result, useAlphaAsMask, matchColor);
}

std::vector<cv::Rect> MatchObject(QImage &img, cv::CascadeClassifier &cascade,
				  double scaleFactor, int minNeighbors,
				  const cv::Size &minSize,
				  const cv::Size &maxSize)
{
	if (img.isNull() || cascade.empty()) {
		return {};
	}

	auto image = QImageToMat(img);
	cv::UMat frameGray;
	cv::cvtColor(image, frameGray, cv::COLOR_RGBA2GRAY);
	cv::equalizeHist(frameGray, frameGray);
	std::vector<cv::Rect> objects;
	cascade.detectMultiScale(frameGray, objects, scaleFactor, minNeighbors,
				 0, minSize, maxSize);
	return objects;
}

uchar GetAvgBrightness(QImage &img)
{
	if (img.isNull()) {
		return 0;
	}

	auto i = QImageToMat(img);
	cv::Mat hsvImage, rgbImage;
	cv::cvtColor(i, rgbImage, cv::COLOR_RGBA2RGB);
	cv::cvtColor(rgbImage, hsvImage, cv::COLOR_RGB2HSV);
	long long brightnessSum = 0;
	for (int i = 0; i < hsvImage.rows; ++i) {
		for (int j = 0; j < hsvImage.cols; ++j) {
			brightnessSum += hsvImage.at<cv::Vec3b>(i, j)[2];
		}
	}
	return brightnessSum / (hsvImage.rows * hsvImage.cols);
}

cv::UMat PreprocessForOCR(const QImage &image, const QColor &color)
{
	auto mat = QImageToMat(image);

	// Only keep the desired color
	cv::cvtColor(mat, mat, cv::COLOR_RGBA2RGB);
	cv::cvtColor(mat, mat, cv::COLOR_RGB2HSV);
	cv::inRange(mat, cv::Scalar(0, 0, 0),
		    cv::Scalar(color.red(), color.green(), color.blue()), mat);

	// Invert to improve ORC detection
	cv::bitwise_not(mat, mat);

	// Scale image up if selected area is too small
	// Results will probably still be unsatisfying
	if (mat.rows <= 300 || mat.cols <= 300) {
		double scale = 0.;
		if (mat.rows < mat.cols) {
			scale = 300. / mat.rows;
		} else {
			scale = 300. / mat.cols;
		}

		cv::resize(mat, mat,
			   cv::Size(mat.cols * scale, mat.rows * scale),
			   cv::INTER_CUBIC);
	}

	return mat;
}

std::string RunOCR(tesseract::TessBaseAPI *ocr, const QImage &image,
		   const QColor &color)
{
	if (image.isNull()) {
		return "";
	}

#ifdef OCR_SUPPORT
	auto mat = PreprocessForOCR(image, color);
	ocr->SetImage(mat.getMat(cv::ACCESS_READ).data, mat.cols, mat.rows, 1,
		      mat.step);
	ocr->Recognize(0);
	std::unique_ptr<char[]> detectedText(ocr->GetUTF8Text());

	if (!detectedText) {
		return "";
	}
	return detectedText.get();

#else
	return "";
#endif
}

bool ContainsPixelsInColorRange(const QImage &image, const QColor &color,
				double colorDeviationThreshold,
				double totalPixelMatchThreshold)
{
	int totalPixels = image.width() * image.height();
	int matchingPixels = 0;
	int maxColorDiff = static_cast<int>(colorDeviationThreshold * 255.0);

	for (int y = 0; y < image.height(); y++) {
		for (int x = 0; x < image.width(); x++) {
			const auto pixelColor = image.pixelColor(x, y);
			const int diffRed =
				std::abs(pixelColor.red() - color.red());
			const int diffGreen =
				std::abs(pixelColor.green() - color.green());
			const int diffBlue =
				std::abs(pixelColor.blue() - color.blue());

			if (diffRed <= maxColorDiff &&
			    diffGreen <= maxColorDiff &&
			    diffBlue <= maxColorDiff) {
				matchingPixels++;
			}
		}
	}

	double matchPercentage =
		static_cast<double>(matchingPixels) / totalPixels;
	return matchPercentage >= totalPixelMatchThreshold;
}

// Assumption is that QImage uses Format_RGBA8888.
// Conversion from: https://github.com/dbzhang800/QtOpenCV
cv::UMat QImageToMat(const QImage &img)
{
	if (img.isNull()) {
		return cv::UMat();
	}
	auto temp = cv::Mat(img.height(), img.width(), CV_8UC(img.depth() / 8),
			    (uchar *)img.bits(), img.bytesPerLine());
	return temp.getUMat(cv::ACCESS_RW);
}

QImage MatToQImage(const cv::Mat &mat)
{
	if (mat.empty()) {
		return QImage();
	}
	return QImage(mat.data, mat.cols, mat.rows,
		      QImage::Format::Format_RGBA8888);
}

void SetupOpenCL()
{
	if (cv::ocl::haveOpenCL() && !cv::ocl::useOpenCL()) {
		blog(LOG_INFO, "enabled OpenCL support for OpenCV");
		cv::ocl::setUseOpenCL(true);
	}
}

} // namespace advss
