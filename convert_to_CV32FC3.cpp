#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    // 读取原始图片
    cv::Mat image = cv::imread("input.jpg");
    if(image.empty()) {
        std::cout << "Error: Could not read the image." << std::endl;
        return -1;
    }

    // 转换为CV_32FC3格式
    cv::Mat float_image;
    image.convertTo(float_image, CV_32FC3, 1.0/255.0);

    // 验证格式
    std::cout << "转换后图片格式: " << float_image.type() << std::endl;
    std::cout << "CV_32FC3的值: " << CV_32FC3 << std::endl;
    if(float_image.type() == CV_32FC3) {
        std::cout << "成功转换为CV_32FC3格式" << std::endl;
    }

    // 显示一些像素值来验证
    std::cout << "第一个像素值: " << float_image.at<cv::Vec3f>(0,0) << std::endl;

    return 0;
}