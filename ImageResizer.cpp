#include "ImageResizer.h"
#include <QFileDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QThread>
#include <QLabel>
#include <QTabWidget>
#include <chrono>
#include <numeric>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <opencv2/core/hal/intrin.hpp>
using namespace std::chrono;

ImageResizer::ImageResizer(QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();
    setAcceptDrops(true);
}

void ImageResizer::setupUI()
{
    QFont font;
    font.setFamily("Noto Sans CJK SC"); // 首选 Noto Sans CJK
    if (!font.exactMatch())
    {
        font.setFamily("WenQuanYi Micro Hei"); // 备选文泉驿微米黑
    }
    QApplication::setFont(font);

// 设置编码
#if (QT_VERSION <= QT_VERSION_CHECK(5, 0, 0))
    QTextCodec *codec = QTextCodec::codecForName("UTF-8");
    QTextCodec::setCodecForLocale(codec);
    QTextCodec::setCodecForCStrings(codec);
    QTextCodec::setCodecForTr(codec);
#endif
    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto *mainLayout = new QVBoxLayout(centralWidget);
    auto *controlLayout = new QHBoxLayout();
    auto *imageLayout = new QHBoxLayout();

    // 控制面板
    auto *controlGroup = new QGroupBox("控制面板");
    auto *controlsLayout = new QVBoxLayout(controlGroup);

    loadButton = new QPushButton("加载图片");
    convertToGrayButton = new QPushButton("转换为灰度图");
    algorithmSelector = new QComboBox();
    algorithmSelector->addItems({"最近邻(自实现)", "最近邻(OpenCV)", "最近邻(FFmpeg)",
                                 "双线性(自实现)", "双线性(OpenCV)", "双线性(FFmpeg)",
                                 "SIMD优化(仅灰度图)", "性能对比"});

    widthSpinBox = new QSpinBox();
    heightSpinBox = new QSpinBox();
    widthSpinBox->setRange(1, 10000);
    heightSpinBox->setRange(1, 10000);

    processButton = new QPushButton("处理图像");
    progressBar = new QProgressBar();

    controlsLayout->addWidget(loadButton);
    controlsLayout->addWidget(convertToGrayButton);
    controlsLayout->addWidget(new QLabel("算法选择:"));
    controlsLayout->addWidget(algorithmSelector);

    // 在输出宽度和高度spinbox之前添加预设尺寸选择
    auto *presetsCombo = new QComboBox();
    presetsCombo->addItem("自定义");
    presetsCombo->addItem("放大2倍");
    presetsCombo->addItem("缩小至1/2");
    presetsCombo->addItem("1920×1080");
    presetsCombo->addItem("1280×720");

    controlsLayout->addWidget(new QLabel("预设尺寸:"));
    controlsLayout->addWidget(presetsCombo);
    controlsLayout->addWidget(new QLabel("输出宽度:"));
    controlsLayout->addWidget(widthSpinBox);
    controlsLayout->addWidget(new QLabel("输出高度:"));
    controlsLayout->addWidget(heightSpinBox);

    // 添加预设尺寸选择的信号连接
    connect(presetsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [=](int index)
            {
                if (!inputImage.empty())
                {
                    switch (index)
                    {
                    case 0: // 自定义
                        break;
                    case 1: // 放大2倍
                        widthSpinBox->setValue(inputImage.cols * 2);
                        heightSpinBox->setValue(inputImage.rows * 2);
                        break;
                    case 2: // 缩小至1/2
                        widthSpinBox->setValue(inputImage.cols / 2);
                        heightSpinBox->setValue(inputImage.rows / 2);
                        break;
                    case 3: // 1920×1080
                        widthSpinBox->setValue(1920);
                        heightSpinBox->setValue(1080);
                        break;
                    case 4: // 1280×720
                        widthSpinBox->setValue(1280);
                        heightSpinBox->setValue(720);
                        break;
                    }
                }
            });

    controlsLayout->addWidget(processButton);
    controlsLayout->addWidget(progressBar);

    controlLayout->addWidget(controlGroup);

    // 图像显示区域
    inputScrollArea = new QScrollArea();
    outputScrollArea = new QScrollArea();
    // outputScrollArea2 = new QScrollArea();

    inputImageLabel = new QLabel("拖放图片到这里");
    inputImageLabel->setAlignment(Qt::AlignCenter);
    outputImageLabel = new QLabel("处理结果");
    outputImageLabel->setAlignment(Qt::AlignCenter);
    // outputImageLabel2 = new QLabel("对比结果");

    // 设置固定大小的显示区域
    inputScrollArea->setFixedSize(1000, 800);
    outputScrollArea->setFixedSize(1000, 800);
    // outputScrollArea2->setFixedSize(400, 300);

    // 设置滚动区域属性
    inputScrollArea->setWidget(inputImageLabel);
    outputScrollArea->setWidget(outputImageLabel);
    // outputScrollArea2->setWidget(outputImageLabel2);

    // 允许滚动
    inputScrollArea->setWidgetResizable(false);
    outputScrollArea->setWidgetResizable(false);
    // outputScrollArea2->setWidgetResizable(false);

    imageLayout->addWidget(inputScrollArea);
    imageLayout->addWidget(outputScrollArea);
    // imageLayout->addWidget(outputScrollArea2);

    mainLayout->addLayout(controlLayout);
    mainLayout->addLayout(imageLayout);

    // 添加结果显示标签页
    // resultTabs = new QTabWidget();
    // imageLayout->addWidget(resultTabs);

    // 添加时间统计标签
    timeLabel = new QLabel();
    controlsLayout->addWidget(timeLabel);

    connect(loadButton, &QPushButton::clicked, this, &ImageResizer::loadImage);
    connect(processButton, &QPushButton::clicked, this, &ImageResizer::processImage);
    connect(convertToGrayButton, &QPushButton::clicked, this, &ImageResizer::convertToGray);
    connect(widthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ImageResizer::updateOutputSize);
    connect(heightSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ImageResizer::updateOutputSize);

    setMinimumSize(800, 800);
    setWindowTitle("图像缩放算法比较");
}

void ImageResizer::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

void ImageResizer::dropEvent(QDropEvent *event)
{
    if (!inputImage.empty())
    {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "确认", "是否要加载新图片？当前图片将被替换。",
                                      QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::No)
        {
            event->ignore();
            return;
        }
    }

    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls())
    {
        QString filePath = mimeData->urls().at(0).toLocalFile();
        currentImagePath = filePath;
        inputImage = cv::imread(filePath.toStdString(), cv::IMREAD_UNCHANGED);
        if (!inputImage.empty())
        {
            displayImage(inputImage, inputImageLabel);
            widthSpinBox->setValue(inputImage.cols);
            heightSpinBox->setValue(inputImage.rows);
            event->acceptProposedAction();
        }
    }
}

void ImageResizer::loadImage()
{
    if (!inputImage.empty())
    {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "确认", "是否要加载新图片？当前图片将被替换。",
                                      QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::No)
        {
            return;
        }
    }

    QString filePath = QFileDialog::getOpenFileName(this,
                                                    "选择图片", "", "图片文件 (*.png *.jpg *.bmp *.tiff *.exr)");
    if (!filePath.isEmpty())
    {
        originalImage = cv::imread(filePath.toStdString(), cv::IMREAD_UNCHANGED);
        inputImage = originalImage.clone();
        isGrayscale = false;
        convertToGrayButton->setText("转换为灰度图");
        // 启用灰度按钮
        convertToGrayButton->setEnabled(true);
        
        if (!inputImage.empty())
        {
            displayImage(inputImage, inputImageLabel);
            widthSpinBox->setValue(inputImage.cols);
            heightSpinBox->setValue(inputImage.rows);
        }
        else
        {
            QMessageBox::warning(this, "错误", "无法加载图像文件！");
        }
    }
}

void ImageResizer::displayImage(const cv::Mat &image, QLabel *label)
{
    if (image.empty())
    {
        return; // 防止空图像导致崩溃
    }

    cv::Mat rgb;
    cv::cvtColor(image, rgb, cv::COLOR_BGR2RGB);
    QImage qimg(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888);

    // 设置标签大小为图像实际大小
    label->setFixedSize(rgb.cols, rgb.rows);
    label->setPixmap(QPixmap::fromImage(qimg));
}

void ImageResizer::processImage()
{
    if (inputImage.empty())
    {
        QMessageBox::warning(this, "错误", "请先加载图片！");
        return;
    }

    // 重置进度条
    progressBar->setValue(0);

    if (algorithmSelector->currentIndex() == 7) // 性能对比
    {
        // 定义三种测试尺寸
        struct TestSize
        {
            QString name;
            int width;
            int height;
        };

        std::vector<TestSize> testSizes = {
            {"放大2倍", inputImage.cols * 2, inputImage.rows * 2},
            {"缩小1/2", inputImage.cols / 2, inputImage.rows / 2},
            {"长宽比变化", inputImage.cols * 5 / 4, inputImage.rows * 3 / 4}};

        const int RUNS = 30;
        // bool canRunSIMD = false;
        bool canRunSIMD = (inputImage.type() == CV_8UC1);
        int numAlgorithms = canRunSIMD ? 7 : 6;
        // 弹窗显示输入图像的类型,长宽等信息,待用户确认后开始测试
        QString info = QString("输入图像信息：\n类型：%1\n尺寸：%2×%3\n通道数：%4\n是否灰度图：%5\n\n是否开始性能测试？")
                           .arg(inputImage.type() == CV_8UC1 ? "CV_8UC1" : inputImage.type() == CV_8UC3 ? "CV_8UC3"
                                                                       : inputImage.type() == CV_16UC1  ? "CV_16UC1"
                                                                       : inputImage.type() == CV_16UC3  ? "CV_16UC3"
                                                                       : inputImage.type() == CV_32FC1  ? "CV_32FC1"
                                                                       : inputImage.type() == CV_32FC3  ? "CV_32FC3"
                                                                                                        : "未知")
                           .arg(inputImage.cols)
                           .arg(inputImage.rows)
                           .arg(inputImage.channels())
                           .arg(isGrayscale ? "是" : "否");

        if (QMessageBox::question(this, "确认", info, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
        {
            // 清空之前的处理时间
            processingTimes.clear();
        }
        else
        {
            return;
        }

        // 存储每种尺寸的性能数据
        std::vector<std::vector<std::vector<double>>> allTimings(testSizes.size());

        // 对每种尺寸进行测试
        for (size_t sizeIndex = 0; sizeIndex < testSizes.size(); ++sizeIndex)
        {
            allTimings[sizeIndex].resize(numAlgorithms);

            for (int run = 0; run < RUNS; ++run)
            {
                for (int algIndex = 0; algIndex < numAlgorithms; ++algIndex)
                {
                    int new_width = testSizes[sizeIndex].width;
                    int new_height = testSizes[sizeIndex].height;

                    high_resolution_clock::time_point t1 = high_resolution_clock::now();

                    switch (algIndex)
                    {
                    case 0: // 最近邻(自实现)
                        switch (inputImage.type())
                        {
                        case CV_8UC1:
                            outputImage = nearestNeighborResizeParallel<u_char>(inputImage, new_width, new_height);
                            break;
                        case CV_8UC3:
                            outputImage = nearestNeighborResizeParallel<uchar>(inputImage, new_width, new_height);
                            break;
                        case CV_16UC1:
                        case CV_16UC3:
                            outputImage = nearestNeighborResizeParallel<uint16_t>(inputImage, new_width, new_height);
                            break;
                        case CV_32FC1:
                        case CV_32FC3:
                            outputImage = nearestNeighborResizeParallel<float>(inputImage, new_width, new_height);
                            break;
                        default:
                            std::cerr << "不支持的图像类型！" << endl;
                        }
                        break;
                    case 1: // 最近邻(OpenCV)
                        outputImage = opencvResize(inputImage, new_width, new_height, cv::INTER_NEAREST);
                        break;
                    case 2: // 最近邻(FFmpeg)
                        outputImage = ffmpegResize(inputImage, new_width, new_height, true);
                        break;
                    case 3: // 双线性(自实现)
                        switch (inputImage.type())
                        {
                        case CV_8UC1:
                        case CV_8UC3:
                            outputImage = bilinearResizeParallel<uchar>(inputImage, new_width, new_height);
                            break;
                        case CV_16UC1:
                        case CV_16UC3:
                            outputImage = bilinearResizeParallel<uint16_t>(inputImage, new_width, new_height);
                            break;
                        case CV_32FC1:
                        case CV_32FC3:
                            outputImage = bilinearResizeParallel<float>(inputImage, new_width, new_height);
                            break;
                        default:
                            std::cerr << "不支持的图像类型！" << endl;
                        }
                        break;
                    case 4: // 双线性(OpenCV)
                        outputImage = opencvResize(inputImage, new_width, new_height, cv::INTER_LINEAR);
                        break;
                    case 5: // 双线性(FFmpeg)
                        outputImage = ffmpegResize(inputImage, new_width, new_height, false);
                        break;
                    case 6: // SIMD优化
                        // 仅支持单通道8位图像
                        if (inputImage.type() == CV_8UC1)
                        {
                            outputImage = simdResize(inputImage, new_width, new_height);
                        }
                        else
                        {
                            QMessageBox::warning(this, "错误", "SIMD优化仅支持单通道8位图像！");
                        }
                        break;
                    default:
                        break;
                    }

                    high_resolution_clock::time_point t2 = high_resolution_clock::now();
                    duration<double, std::milli> time_span = t2 - t1;
                    allTimings[sizeIndex][algIndex].push_back(time_span.count());

                    // 显示进度
                    progressBar->setValue((sizeIndex * RUNS * numAlgorithms + run * numAlgorithms + algIndex + 1) * 100 /
                                          (testSizes.size() * RUNS * numAlgorithms));

                    // 更新时间标签
                    timeLabel->setText(QString("已完成 %1/%2 次测试").arg(sizeIndex * RUNS * numAlgorithms + run * numAlgorithms + algIndex + 1).arg(testSizes.size() * RUNS * numAlgorithms));
                    qApp->processEvents();

                    // 释放输出图像
                    outputImage.release();
                }
            }
        }

        // 显示结果对话框
        QDialog *resultDialog = new QDialog(this);
        resultDialog->setWindowTitle("性能对比结果");
        resultDialog->resize(1000, 600);

        QVBoxLayout *dialogLayout = new QVBoxLayout(resultDialog);

        // 创建柱状图
        QBarSeries *series = new QBarSeries();

        // 为每种尺寸创建数据集
        for (size_t sizeIndex = 0; sizeIndex < testSizes.size(); ++sizeIndex)
        {
            QBarSet *barSet = new QBarSet(testSizes[sizeIndex].name);
            barSet->setColor(QColor(sizeIndex == 0 ? "#FF4444" : sizeIndex == 1 ? "#44FF44"
                                                                                : "#4444FF"));

            // 计算每个算法的平均时间
            for (size_t algIndex = 0; algIndex < allTimings[sizeIndex].size(); ++algIndex)
            {
                if (allTimings[sizeIndex][algIndex].empty())
                    continue;
                double avgTime = std::accumulate(
                                     allTimings[sizeIndex][algIndex].begin(),
                                     allTimings[sizeIndex][algIndex].end(), 0.0) /
                                 RUNS;
                *barSet << std::max(0.0, avgTime); // 确保时间不为负
            }

            series->append(barSet);
        }

        QChart *chart = new QChart();
        chart->addSeries(series);
        chart->setTitle("不同尺寸下的算法性能对比");
        chart->setAnimationOptions(QChart::SeriesAnimations);

        // 设置坐标轴
        QStringList categories;
        categories << "最近邻(自实现)" << "最近邻(OpenCV)" << "最近邻(FFmpeg)"
                   << "双线性(自实现)" << "双线性(OpenCV)" << "双线性(FFmpeg)";
        if (canRunSIMD)
        {
            categories << "SIMD优化";
        }

        QBarCategoryAxis *axisX = new QBarCategoryAxis();
        axisX->append(categories);
        chart->addAxis(axisX, Qt::AlignBottom);
        series->attachAxis(axisX);

        QValueAxis *axisY = new QValueAxis();
        axisY->setTitleText("时间(毫秒)");
        axisY->setMin(0); // 确保Y轴从0开始
        chart->addAxis(axisY, Qt::AlignLeft);
        series->attachAxis(axisY);

        QChartView *chartView = new QChartView(chart);
        chartView->setRenderHint(QPainter::Antialiasing);

        dialogLayout->addWidget(chartView);

        // 添加图例说明
        QLabel *legendLabel = new QLabel("颜色说明：红色-放大2倍，绿色-缩小1/2，蓝色-长宽比变化");
        dialogLayout->addWidget(legendLabel);

        resultDialog->exec();
    }
    else
    {
        // ... 原有的单一算法处理代码 ...
        int type = inputImage.type();
        int new_width = widthSpinBox->value();
        int new_height = heightSpinBox->value();
        // 计算处理时间
        high_resolution_clock::time_point t1 = high_resolution_clock::now();
        switch (algorithmSelector->currentIndex())
        {
        case 0: // 最近邻(自实现)
            switch (type)
            {
            case CV_8UC1:
            case CV_8UC3:
                outputImage = nearestNeighborResizeParallel<uchar>(inputImage, new_width, new_height);
                break;
            case CV_16UC1:
            case CV_16UC3:
                outputImage = nearestNeighborResizeParallel<uint16_t>(inputImage, new_width, new_height);
                break;
            case CV_32FC1:
            case CV_32FC3:
                outputImage = nearestNeighborResizeParallel<float>(inputImage, new_width, new_height);
                break;
            default:
                std::cerr << "不支持的图像类型！" << endl;
            }
            break;
        case 1: // 最近邻(OpenCV)
            outputImage = opencvResize(inputImage, new_width, new_height, cv::INTER_NEAREST);
            break;
        case 2: // 最近邻(FFmpeg)

            outputImage = ffmpegResize(inputImage, new_width, new_height, true);
            break;
        case 3: // 双线性(自实现)
            switch (type)
            {
            case CV_8UC1:
            case CV_8UC3:
                outputImage = bilinearResizeParallel<uchar>(inputImage, new_width, new_height);
                break;
            case CV_16UC1:
            case CV_16UC3:
                outputImage = bilinearResizeParallel<uint16_t>(inputImage, new_width, new_height);
                break;
            case CV_32FC1:
            case CV_32FC3:
                outputImage = bilinearResizeParallel<float>(inputImage, new_width, new_height);
                break;
            default:
                std::cerr << "不支持的图像类型！" << endl;
            }
            break;
        case 4: // 双线性(OpenCV)
            outputImage = opencvResize(inputImage, new_width, new_height, cv::INTER_LINEAR);
            break;
        case 5: // 双线性(FFmpeg)
            outputImage = ffmpegResize(inputImage, new_width, new_height, false);
            break;
        case 6: // SIMD优化
            // 仅支持单通道8位图像
            if (type == CV_8UC1)
            {
                outputImage = simdResize(inputImage, new_width, new_height);
            }
            else
            {
                QMessageBox::warning(this, "错误", "SIMD优化仅支持单通道8位图像！");
            }
            break;
        default:
            break;
        }
        high_resolution_clock::time_point t2 = high_resolution_clock::now();
        duration<double, std::milli> time_span = t2 - t1;
        // 展示处理结果
        displayImage(outputImage, outputImageLabel);
        // 更新处理时间
        timeLabel->setText(QString("处理时间: %1 毫秒").arg(time_span.count()));
    }
}

void ImageResizer::convertToGray()
{
    if (inputImage.empty())
    {
        QMessageBox::warning(this, "错误", "请先加载图片！");
        return;
    }

    try
    {
        // 保存原始图像
        if (!isGrayscale)
        {
            originalImage = inputImage.clone();
        }

        cv::Mat grayImage;
        cv::cvtColor(inputImage, grayImage, cv::COLOR_BGR2GRAY);

        if (!grayImage.empty())
        {
            inputImage = grayImage.clone();
            isGrayscale = true;
            displayImage(inputImage, inputImageLabel);

            // 更新按钮文本
            convertToGrayButton->setText("已经是灰度图");
            // 禁止点击
            convertToGrayButton->setEnabled(false);
        }
    }
    catch (const cv::Exception &e)
    {
        QMessageBox::warning(this, "错误", "转换灰度图失败：" + QString(e.what()));
    }
}

void ImageResizer::updateResults()
{
    resultTabs->clear();

    if (algorithmSelector->currentIndex() == 7) // 性能对比
    {
        QWidget *tab = new QWidget();
        QVBoxLayout *layout = new QVBoxLayout(tab);

        // 创建柱状图
        QBarSeries *series = new QBarSeries();
        QBarSet *timeSet = new QBarSet("处理时间(ms)");

        for (size_t i = 0; i < processingTimes.size(); ++i)
        {
            *timeSet << processingTimes[i];
        }

        series->append(timeSet);

        QChart *chart = new QChart();
        chart->addSeries(series);
        chart->setTitle("算法性能对比");
        chart->setAnimationOptions(QChart::SeriesAnimations);

        // 设置坐标轴
        QStringList categories;
        categories << "最近邻(自实现)" << "最近邻(OpenCV)" << "最近邻(FFmpeg)"
                   << "双线性(自实现)" << "双线性(OpenCV)" << "双线性(FFmpeg)";
        if (inputImage.type() == CV_8UC1)
        {
            categories << "SIMD优化";
        }

        QBarCategoryAxis *axisX = new QBarCategoryAxis();
        axisX->append(categories);
        chart->addAxis(axisX, Qt::AlignBottom);
        series->attachAxis(axisX);

        QValueAxis *axisY = new QValueAxis();
        axisY->setTitleText("时间(毫秒)");
        chart->addAxis(axisY, Qt::AlignLeft);
        series->attachAxis(axisY);

        // 创建图表视图
        QChartView *chartView = new QChartView(chart);
        chartView->setRenderHint(QPainter::Antialiasing);
        chartView->setMinimumSize(800, 400);

        layout->addWidget(chartView);
        resultTabs->addTab(tab, "性能对比结果");
    }
    else
    {
        // ...existing code for normal image display...
    }
}

void ImageResizer::updateOutputSize(int)
{
    // 可以在这里添加输出尺寸更新的相关逻辑
}

// 在 ImageResizer 类中添加算法实现
template <typename T>
cv::Mat ImageResizer::nearestNeighborResizeParallel(const cv::Mat &input_image, int output_width, int output_height)
{
    int input_width = input_image.cols;
    int input_height = input_image.rows;

    double scale_width, scale_height;
    calculateScalingFactors(input_width, input_height, output_width, output_height, scale_width, scale_height);

    cv::Mat output_image(output_height, output_width, input_image.type());

    parallel_for_(cv::Range(0, output_height), [&](const cv::Range &range)
                  {
        for (int y_dst = range.start; y_dst < range.end; ++y_dst) {
            T *output_row = output_image.ptr<T>(y_dst);
            int y_src = static_cast<int>(y_dst * scale_height + 0.5);
            y_src = std::min(std::max(y_src, 0), input_height - 1);

            for (int x_dst = 0; x_dst < output_width; ++x_dst) {
                int x_src = static_cast<int>(x_dst * scale_width + 0.5);
                x_src = std::min(std::max(x_src, 0), input_width - 1);

                if (input_image.channels() == 1) {
                    output_row[x_dst] = input_image.at<T>(y_src, x_src);
                } else if (input_image.channels() == 3) {
                    cv::Vec<T, 3> pixel = input_image.at<cv::Vec<T, 3>>(y_src, x_src);
                    output_image.at<cv::Vec<T, 3>>(y_dst, x_dst) = pixel;
                }
            }
        } });

    return output_image;
}

template <typename T>
cv::Mat ImageResizer::bilinearResizeParallel(const cv::Mat &input_image, int output_width, int output_height)
{
    int input_width = input_image.cols;
    int input_height = input_image.rows;

    double scale_width = static_cast<double>(input_width) / output_width;
    double scale_height = static_cast<double>(input_height) / output_height;

    cv::Mat output_image(output_height, output_width, input_image.type());
    using namespace cv;
    using namespace std;
    parallel_for_(Range(0, output_height), [&](const Range &range)
                  {
        for (int y_dst = range.start; y_dst < range.end; ++y_dst) {
            double y_src = (y_dst + 0.5) * scale_height - 0.5;
            int y0 = static_cast<int>(floor(y_src));
            int y1 = y0 + 1;
            double y_lerp = y_src - y0;

            y0 = min(max(y0, 0), input_height - 1);
            y1 = min(max(y1, 0), input_height - 1);

            for (int x_dst = 0; x_dst < output_width; ++x_dst) {
                double x_src = (x_dst + 0.5) * scale_width - 0.5;
                int x0 = static_cast<int>(floor(x_src));
                int x1 = x0 + 1;
                double x_lerp = x_src - x0;

                x0 = min(max(x0, 0), input_width - 1);
                x1 = min(max(x1, 0), input_width - 1);

                if (input_image.channels() == 1) {
                    T p00 = input_image.at<T>(y0, x0);
                    T p01 = input_image.at<T>(y0, x1);
                    T p10 = input_image.at<T>(y1, x0);
                    T p11 = input_image.at<T>(y1, x1);

                    double value = (1 - y_lerp) * ((1 - x_lerp) * p00 + x_lerp * p01) +
                                   y_lerp * ((1 - x_lerp) * p10 + x_lerp * p11);

                    if (std::is_same<T, uchar>::value || std::is_same<T, uint16_t>::value) {
                        output_image.at<T>(y_dst, x_dst) = static_cast<T>(round(value));
                    } else {
                        output_image.at<T>(y_dst, x_dst) = static_cast<T>(value);
                    }
                } else if (input_image.channels() == 3) {
                    Vec<T, 3> p00 = input_image.at<Vec<T, 3>>(y0, x0);
                    Vec<T, 3> p01 = input_image.at<Vec<T, 3>>(y0, x1);
                    Vec<T, 3> p10 = input_image.at<Vec<T, 3>>(y1, x0);
                    Vec<T, 3> p11 = input_image.at<Vec<T, 3>>(y1, x1);

                    Vec<double, 3> value;
                    for (int c = 0; c < 3; ++c) {
                        value[c] = (1 - y_lerp) * ((1 - x_lerp) * p00[c] + x_lerp * p01[c]) +
                                   y_lerp * ((1 - x_lerp) * p10[c] + x_lerp * p11[c]);

                        if (std::is_same<T, uchar>::value || std::is_same<T, uint16_t>::value) {
                            output_image.at<Vec<T, 3>>(y_dst, x_dst)[c] = static_cast<T>(round(value[c]));
                        } else {
                            output_image.at<Vec<T, 3>>(y_dst, x_dst)[c] = static_cast<T>(value[c]);
                        }
                    }
                }
            }
        } });

    return output_image;
}

// 辅助函数实现
void ImageResizer::calculateScalingFactors(int input_width, int input_height,
                                           int output_width, int output_height,
                                           double &scale_width, double &scale_height)
{
    scale_width = static_cast<double>(input_width) / output_width;
    scale_height = static_cast<double>(input_height) / output_height;
}

cv::Mat ImageResizer::opencvResize(const cv::Mat &input_image, int output_width, int output_height, int interpolation)
{
    cv::Mat output_image;
    cv::resize(input_image, output_image, cv::Size(output_width, output_height), 0, 0, interpolation);
    return output_image;
}

cv::Mat ImageResizer::ffmpegResize(const cv::Mat &input, int newWidth, int newHeight, bool useNearest)
{
    if (input.empty())
    {
        return cv::Mat();
    }

    // 确定输入格式
    AVPixelFormat input_pix_fmt;
    if (input.channels() == 1)
    {
        input_pix_fmt = AV_PIX_FMT_GRAY8;
    }
    else if (input.channels() == 3)
    {
        input_pix_fmt = AV_PIX_FMT_BGR24;
    }
    else
    {
        throw std::runtime_error("不支持的图像格式");
    }

    // 设置输出格式与输入相同
    AVPixelFormat output_pix_fmt = input_pix_fmt;

    // 创建转换上下文
    SwsContext *sws_ctx = sws_getContext(
        input.cols, input.rows, input_pix_fmt,
        newWidth, newHeight, output_pix_fmt,
        useNearest ? SWS_POINT : SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    if (!sws_ctx)
    {
        throw std::runtime_error("无法创建FFmpeg缩放上下文");
    }

    // 创建输出图像
    cv::Mat output(newHeight, newWidth, input.type());

    // 设置数据指针
    const uint8_t *srcSlice[4] = {input.data, nullptr, nullptr, nullptr};
    int srcStride[4] = {static_cast<int>(input.step[0]), 0, 0, 0};

    uint8_t *dstSlice[4] = {output.data, nullptr, nullptr, nullptr};
    int dstStride[4] = {static_cast<int>(output.step[0]), 0, 0, 0};

    // 执行缩放
    sws_scale(sws_ctx, srcSlice, srcStride, 0, input.rows,
              dstSlice, dstStride);

    // 清理
    sws_freeContext(sws_ctx);

    return output;
}

cv::Mat ImageResizer::simdResize(const cv::Mat &input_image, int output_width, int output_height)
{
// 检查平台SIMD支持
#if defined(__x86_64__) || defined(_M_X64)
// x86_64架构
#if CV_SIMD128
    const int vec_size = cv::v_uint8::nlanes;
#else
    const int vec_size = 1; // 降级到标量处理
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
// ARM64架构
#if CV_SIMD128
    const int vec_size = cv::v_uint8::nlanes;
#else
    const int vec_size = 1;
#endif
#else
    // 其他平台降级到标量处理
    const int vec_size = 1;
#endif

    cv::Mat output_image(output_height, output_width, CV_8UC1);
    double scale_width = static_cast<double>(input_image.cols) / output_width;
    double scale_height = static_cast<double>(input_image.rows) / output_height;

    // 根据SIMD支持选择处理方式
    if (vec_size > 1)
    {
        // 使用SIMD的处理逻辑
        int vec_size = cv::v_uint8::nlanes;

        cv::parallel_for_(cv::Range(0, output_height), [&](const cv::Range &range)
                          {
            for (int y_dst = range.start; y_dst < range.end; ++y_dst) {
                uchar *output_row = output_image.ptr<uchar>(y_dst);
                int y_src = static_cast<int>(y_dst * scale_height + 0.5);
                y_src = std::min(std::max(y_src, 0), input_image.rows - 1);
                const uchar *input_row = input_image.ptr<uchar>(y_src);

                int x_dst = 0;
                for (; x_dst <= output_width - vec_size; x_dst += vec_size) {
                    int indices[vec_size];
                    for (int i = 0; i < vec_size; ++i) {
                        int x_src = static_cast<int>((x_dst + i) * scale_width + 0.5);
                        indices[i] = std::min(std::max(x_src, 0), input_image.cols - 1);
                    }
                    cv::v_uint8 pixels = cv::v_lut(input_row, indices);
                    cv::v_store(output_row + x_dst, pixels);
                }

                for (; x_dst < output_width; ++x_dst) {
                    int x_src = static_cast<int>(x_dst * scale_width + 0.5);
                    x_src = std::min(std::max(x_src, 0), input_image.cols - 1);
                    output_row[x_dst] = input_row[x_src];
                }
            } });
    }
    else
    {
        // 降级到标量处理
        cv::parallel_for_(cv::Range(0, output_height), [&](const cv::Range &range)
                          {
            for (int y_dst = range.start; y_dst < range.end; ++y_dst) {
                uchar *output_row = output_image.ptr<uchar>(y_dst);
                int y_src = static_cast<int>(y_dst * scale_height + 0.5);
                y_src = std::min(std::max(y_src, 0), input_image.rows - 1);
                const uchar *input_row = input_image.ptr<uchar>(y_src);

                for (int x_dst = 0; x_dst < output_width; ++x_dst) {
                    int x_src = static_cast<int>(x_dst * scale_width + 0.5);
                    x_src = std::min(std::max(x_src, 0), input_image.cols - 1);
                    output_row[x_dst] = input_row[x_src];
                }
            } });
    }

    return output_image;
}