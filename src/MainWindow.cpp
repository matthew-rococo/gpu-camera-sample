/*
 Copyright 2011-2019 Fastvideo, LLC.
 All rights reserved.

 This file is a part of the GPUCameraSample project
 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
 3. Any third-party SDKs from that project (XIMEA SDK, Fastvideo SDK, etc.) are licensed on different terms. Please see their corresponding license terms.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 The views and conclusions contained in the software and documentation are those
 of the authors and should not be interpreted as representing official policies,
 either expressed or implied, of the FreeBSD Project.
*/

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QTimer>
#include <QFileDialog>
#include <QMessageBox>

#include <iterator>

#include "ppm.h"
#include "PGMCamera.h"
#include "RawProcessor.h"
#include "FPNReader.h"
#include "FFCReader.h"

#ifdef SUPPORT_XIMEA
#include "XimeaCamera.h"
#endif

QVector<unsigned short> gammaLin(16384);
QVector<unsigned short> gammaSRGB(16384);

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    int devCount = 0;
    cudaGetDeviceCount(&devCount);

    if(devCount == 0)
    {
        return;
    }

    mRendererPtr.reset(new GLRenderer());

    mMediaViewer.reset(new GLImageViewer(mRendererPtr.data()));
    mContainerPtr.reset(QWidget::createWindowContainer(mMediaViewer.data()));
    mContainerPtr->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->MediaViewerLayout->insertWidget(0, mContainerPtr.data());
    mContainerPtr->setMinimumSize(QSize(100, 100));
    mContainerPtr->setFocusPolicy(Qt::NoFocus);
    mRendererPtr->setRenderWnd(mMediaViewer.data());


    for(int i = 0; i < 16384; i++)
    {
        gammaLin[i] = static_cast<unsigned short>(i * 4);

        double y = 0;
        double x = i * 4;
        x /= 65535.;
        if(x <= 0.0031308)
            y = x * 12.92;
        else
            y = 1.055 * pow(x, 1.0/2.4) - 0.055;

        gammaSRGB[i] = static_cast<unsigned short>(y * 65535);
    }

    cudaDeviceProp devProps;
    for(int i = 0; i < devCount; i++)
    {
        if(cudaGetDeviceProperties(&devProps, i) != cudaSuccess)
            continue;
        ui->cboCUDADevice->addItem(QString::fromLatin1(devProps.name), QVariant(i));
    }
    {
        QSignalBlocker b(ui->cboBayerPattern);
        ui->cboBayerPattern->addItem("RGGB", FAST_BAYER_RGGB);
        ui->cboBayerPattern->addItem("BGGR", FAST_BAYER_BGGR);
        ui->cboBayerPattern->addItem("GBRG", FAST_BAYER_GBRG);
        ui->cboBayerPattern->addItem("GRBG", FAST_BAYER_GRBG);
        ui->cboBayerPattern->setCurrentIndex(0);
    }
    {
        QSignalBlocker b(ui->cboBayerType);
        ui->cboBayerType->addItem(QStringLiteral("HQLI, 5x5"), FAST_HQLI);
        ui->cboBayerType->addItem(QStringLiteral("DFPD, 11x11"), FAST_DFPD);
        ui->cboBayerType->addItem(QStringLiteral("MG, 23x23"), FAST_MG);
    }
    {
        QSignalBlocker b(ui->cboSamplingFmt);
        ui->cboSamplingFmt->addItem(QStringLiteral("420"), JPEG_420);
        ui->cboSamplingFmt->addItem(QStringLiteral("422"), JPEG_422);
        ui->cboSamplingFmt->addItem(QStringLiteral("444"), JPEG_444);
    }
    {
        QSignalBlocker b(ui->cboOutFormat);
        ui->cboOutFormat->addItem(QStringLiteral("JPEG"), CUDAProcessorOptions::vcJPG);
        ui->cboOutFormat->addItem(QStringLiteral("Motion JPEG"), CUDAProcessorOptions::vcMJPG);
        ui->cboOutFormat->addItem(QStringLiteral("PGM"), CUDAProcessorOptions::vcPGM);
    }

    QSignalBlocker b3(ui->cboGamma);
    ui->cboGamma->addItem("Linear", ogLinear);
    ui->cboGamma->addItem("sRGB", ogsRGB);
    ui->cboGamma->setCurrentIndex(0);

    mMediaViewer->setViewMode(ui->chkZoomFit->isChecked() ? GLImageViewer::vmZoomFit : GLImageViewer::vmPan);
    connect(mMediaViewer.data(), SIGNAL(zoomChanged(qreal)), this, SLOT(onZoomChanged(qreal)));

    //Denoise
    connect(ui->denoiseCtlr, SIGNAL(stateChanged(bool)), this, SLOT(onDenoiseStateChanged(bool)));
    connect(ui->denoiseCtlr, SIGNAL(thresholdTypeChanged(int)), this, SLOT(onThresholdTypeChanged(int)));
    connect(ui->denoiseCtlr, SIGNAL(waveletTypeChanged(int)), this, SLOT(onWaveletTypeChanged(int)));
    connect(ui->denoiseCtlr, SIGNAL(yThresholdChanged(float)), this, SLOT(onYThresholdChanged(float)));
    connect(ui->denoiseCtlr, SIGNAL(cbThresholdChanged(float)), this, SLOT(onCbThresholdChanged(float)));
    connect(ui->denoiseCtlr, SIGNAL(shrinkageChanged(int)), this, SLOT(onShrinkageChanged(int)));
    connect(ui->denoiseCtlr, SIGNAL(paramsChanged()), this, SLOT(onDenoiseParamsChanged()));

    mStatusLabel = new QLabel(this);
    mFpsLabel = new QLabel(this);

    ui->statusBar->addWidget(mStatusLabel);
    ui->statusBar->addWidget(mFpsLabel);

    QToolButton* openButton=
                dynamic_cast<QToolButton*>(ui->mainToolBar->widgetForAction(ui->actionOpenBayerPGM));
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->addAction(ui->actionOpenGrayPGM);

    qRegisterMetaType<CameraBase::cmrCameraState>("cmrCameraState");

#ifdef SUPPORT_XIMEA
    ui->mainToolBar->insertAction(ui->actionOpenBayerPGM, ui->actionOpenCamera);
    ui->menuCamera->insertAction(ui->actionOpenBayerPGM, ui->actionOpenCamera);
#endif
    QTimer::singleShot(0, this, [this](){delayInit();});
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::delayInit()
{
    readSettings();
    onCameraStateChanged(CameraBase::cstClosed);
    mRendererPtr->update();
}

void MainWindow::customEvent(QEvent* event)
{
    if(event->type() == FrameEventID)
    {
        if(!mProcessorPtr || !mCameraPtr)
            return;

        mProcessorPtr->wake();
    }

    QMainWindow::customEvent(event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    writeSettings();

    if(mCameraPtr)
        mCameraPtr->stop();

    if(mProcessorPtr)
        mProcessorPtr->stop();

    QMainWindow::closeEvent(event);

}

void MainWindow::initNewCamera(CameraBase* cmr, uint32_t devID)
{
    mCameraPtr.reset(cmr);
    if(!mCameraPtr)
        return;

    connect(mCameraPtr.data(),
            SIGNAL(stateChanged(CameraBase::cmrCameraState)),
            this,
            SLOT(onCameraStateChanged(CameraBase::cmrCameraState)));

    if(!mCameraPtr->open(devID))
        return;

    mProcessorPtr.reset(new RawProcessor(mCameraPtr.data(), mRendererPtr.data()));

    connect(mProcessorPtr.data(), SIGNAL(finished()), this, SLOT(onGPUFinished()));
    connect(mProcessorPtr.data(), SIGNAL(error()), this, SLOT(onGPUError()));

    mCameraPtr->setProcessor(mProcessorPtr.data());
    {
        QSignalBlocker b(ui->cboBayerPattern);
        ui->cboBayerPattern->setCurrentIndex(ui->cboBayerPattern->findData(mCameraPtr->bayerPattern()));
    }

    mOptions.Width = mCameraPtr->width();
    mOptions.Height = mCameraPtr->height();
    mOptions.MaxWidth = mCameraPtr->width();
    mOptions.MaxHeight = mCameraPtr->height();
    mOptions.BayerFormat = mCameraPtr->bayerPattern();
    mOptions.SurfaceFmt = mCameraPtr->surfaceFormat();
    mOptions.WhiteLevel = mCameraPtr->whiteLevel();
    mOptions.BlackLevel = 0;
    mOptions.Packed = mCameraPtr->isPacked();
    updateOptions(mOptions);

    int bpp = GetBitsPerChannelFromSurface(mCameraPtr->surfaceFormat());

    QString msg = QString(QStringLiteral("%1 %2, s\\n: %3 Width: %4, Height: %5, Pixel format: %6 bpp%7")).
            arg(mCameraPtr->manufacturer()).
            arg(mCameraPtr->model()).
            arg(mCameraPtr->serial()).
            arg(mOptions.Width).
            arg(mOptions.Height).
            arg(bpp).
            arg(mCameraPtr->isPacked() ? QStringLiteral(" packed") : QString());

    mStatusLabel->setText(msg);

    mRendererPtr->setImageSize(QSize(mOptions.Width, mOptions.Height));
    on_chkZoomFit_toggled(ui->chkZoomFit->isChecked());

    ui->actionPlay->setChecked(true);
}

void MainWindow::openCamera(uint32_t devID)
{
#ifdef SUPPORT_XIMEA
    if(mCameraPtr)
        mCameraPtr->stop();

    initNewCamera(new XimeaCamera(), devID);
#else
    Q_UNUSED(devID)
#endif
}

void MainWindow::openPGMFile(bool isBayer)
{
    QString fileName = QFileDialog::getOpenFileName(this,
                                                    QStringLiteral("Select pgm file"),
                                                    QDir::homePath(),
                                                    QStringLiteral("Images (*.pgm)"));

    if(fileName.isEmpty())
        return;

    if(mCameraPtr)
        mCameraPtr->stop();

    initNewCamera(new PGMCamera(
                      fileName,
                      (fastBayerPattern_t)ui->cboBayerPattern->currentData().toInt(),
                      isBayer),
                  0);

}

void MainWindow::raw2Rgb(bool update, bool init)
{
    if(!mProcessorPtr || !mCameraPtr)
        return;
    mProcessorPtr->updateOptions(mOptions);
    if(init)
        mProcessorPtr->init();

    mProcessorPtr->wake();

    if(update)
        updateAll();
}

void MainWindow::onZoomChanged(qreal zoom)
{
    QSignalBlocker blocker(ui->sldZoom);
    ui->lblZoom->setText(QString("%1%").arg((int)(zoom * 100)));
    ui->sldZoom->setValue(zoom * 100);
}

void MainWindow::on_chkZoomFit_toggled(bool checked)
{
    if(checked)
    {
        mMediaViewer->setViewMode(GLImageViewer::vmZoomFit);
        ui->btnResetZoom->setEnabled(false);
        ui->sldZoom->setEnabled(false);
        ui->chkZoomFit->setChecked(true);
    }
    else
    {
        mMediaViewer->setViewMode(GLImageViewer::vmPan);
        ui->btnResetZoom->setEnabled(true);
        ui->sldZoom->setEnabled(true);
        ui->chkZoomFit->setChecked(false);
    }
}

void MainWindow::on_sldZoom_valueChanged(int value)
{
    mMediaViewer->setZoom((float)value / 100);
    ui->lblZoom->setText(QString("%1%").arg(value));
}

void MainWindow::on_btnResetZoom_clicked()
{
    if(ui->chkZoomFit->isChecked())
        return;
    ui->sldZoom->setValue(100);
}

void MainWindow::onDenoiseParamsChanged()
{
    if(!mProcessorPtr)
        return;

    bool enable = false;
    ui->denoiseCtlr->getDenoiseParams(mOptions.DenoiseParams, enable);
    mOptions.EnableDenoise = enable;
    raw2Rgb();
}

void MainWindow::onDenoiseStateChanged(bool on)
{
    if(!mProcessorPtr)
        return;

    mOptions.EnableDenoise = on;

    raw2Rgb();
}

void MainWindow::onThresholdTypeChanged(int newType)
{
    if(!mProcessorPtr)
        return;

    mOptions.DenoiseStaticParams.function = (fastDenoiseThresholdFunctionType_t)newType;


    raw2Rgb(true, true);
}

void MainWindow::onWaveletTypeChanged(int newType)
{
    if(!mProcessorPtr)
        return;

    mOptions.DenoiseStaticParams.wavelet = (fastWaveletType_t)newType;
    raw2Rgb(true, true);
}

void MainWindow::onShrinkageChanged(int newSrinkage)
{
    Q_UNUSED(newSrinkage)

    if(!mProcessorPtr)
        return;

    raw2Rgb(true, true);
}

void MainWindow::onYThresholdChanged(float newThreshold)
{
    if(!mProcessorPtr || !mCameraPtr)
        return;

    mOptions.DenoiseParams.threshold[0] = newThreshold;
    if(mCameraPtr->getFrameBuffer()->surfaceFmt() != FAST_I8)
        mOptions.DenoiseParams.threshold[0]*= 4;
    raw2Rgb();
}

void MainWindow::onCbThresholdChanged(float newThreshold)
{
    if(!mProcessorPtr || !mCameraPtr)
        return;

    mOptions.DenoiseParams.threshold[1] = newThreshold;
    mOptions.DenoiseParams.threshold[2] = newThreshold;
    if(mCameraPtr->getFrameBuffer()->surfaceFmt() != FAST_I8)
    {
        mOptions.DenoiseParams.threshold[1]*= 25;
        mOptions.DenoiseParams.threshold[2]*= 25;
    }
    raw2Rgb();
}

void MainWindow::updateAll()
{
    if(mProcessorPtr && mCameraPtr)
    {

    }
}

void MainWindow::on_sldEV_valueChanged(int value)
{
    if(!mProcessorPtr)
        return;

    mOptions.eV = float(pow(2, double(value) / 100.));
    QString sgn = value > 0 ? QStringLiteral("+") : QString();
    ui->lblEV->setText(sgn + QString::number(double(value) / 100, 'f', 2) + " eV");

    raw2Rgb();
}

void MainWindow::on_btnResetEV_clicked()
{
    ui->sldEV->setValue(0);
}


void MainWindow::on_sldRed_valueChanged(int value)
{
    if(!mProcessorPtr)
        return;

    float r = float(value) / 100;
    ui->lblRed->setText(QString::number(r, 'f', 2));

    mOptions.Red = r;
    raw2Rgb();
}

void MainWindow::on_sldGreen_valueChanged(int value)
{
    if(!mProcessorPtr)
        return;

    float g = float(value) / 100;
    ui->lblGreen->setText(QString::number(g, 'f', 2));

    mOptions.Green = g;
    raw2Rgb();
}

void MainWindow::on_sldBlue_valueChanged(int value)
{
    if(!mProcessorPtr)
        return;

    float b = float(value) / 100;
    ui->lblBlue->setText(QString::number(b, 'f', 2));

    mOptions.Blue = b;
    raw2Rgb();
}

void MainWindow::on_btnResetRed_clicked()
{
    ui->sldRed->setValue(100);
}

void MainWindow::on_btnResetGreen_clicked()
{
    ui->sldGreen->setValue(100);
}

void MainWindow::on_btnResetBlue_clicked()
{
    ui->sldBlue->setValue(100);
}

void MainWindow::readSettings()
{
    QSettings settings;

    restoreGeometry(settings.value("MainWnd/geometry").toByteArray());
    restoreState(settings.value("MainWnd/windowState").toByteArray());

    QString def = QDir::homePath() + "/FastMV/Record";
    ui->txtOutPath->setText(settings.value("Record/OutPath", def).toString());
    ui->txtFilePrefix->setText(settings.value("Record/FilePrefix", QStringLiteral("Frame_")).toString());

    {
        QSignalBlocker b(ui->cboOutFormat);
        ui->cboOutFormat->setCurrentIndex(ui->cboOutFormat->findData(
                                              settings.value("Record/OutFormat", CUDAProcessorOptions::vcJPG)));
    }

    {
        QSignalBlocker b(ui->cboSamplingFmt);
        ui->cboOutFormat->setCurrentIndex(ui->cboOutFormat->findData(
                                              settings.value("Record/SamplingFormat", JPEG_420)));
    }

    {
        QSignalBlocker b(ui->spnJpegQty);
        ui->spnJpegQty->setValue(settings.value("Record/JpegQty", 90).toInt());
    }

}

void MainWindow::writeSettings()
{
    QSettings settings;
    settings.setValue("MainWnd/geometry", saveGeometry());
    settings.setValue("MainWnd/windowState", saveState());

    settings.setValue("Record/FilePrefix", ui->txtFilePrefix->text());
    settings.setValue("Record/OutPath", ui->txtOutPath->text());
    settings.setValue("Record/OutFormat", ui->cboOutFormat->currentData());
    settings.setValue("Record/SamplingFormat", ui->cboOutFormat->currentData());
    settings.setValue("Record/JpegQty", ui->spnJpegQty->value());
}

void MainWindow::on_cboGamma_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    if(!mProcessorPtr)
        return;

    OutputGamma g = (OutputGamma)(ui->cboGamma->currentData().toInt());
    CUDAProcessorBase* proc = mProcessorPtr->getCUDAProcessor();
    if(proc == nullptr)
        return;
    {
        QMutexLocker l(&(proc->mut));
        switch (g)
        {
        case ogLinear:
            std::copy(gammaLin.begin(), gammaLin.end(), std::begin(proc->outLut.lut));
            break;
        case ogsRGB:
            std::copy(gammaSRGB.begin(), gammaSRGB.end(), std::begin(proc->outLut.lut));
            break;
        default:
            return;
        }
    }

    raw2Rgb();
}

void MainWindow::updateOptions(CUDAProcessorOptions& opts)
{
    opts.BayerFormat = fastBayerPattern_t(ui->cboBayerPattern->currentData().toInt());
    opts.BayerType = fastDebayerType_t(ui->cboBayerType->currentData().toInt());
    opts.Red   = (float)(ui->sldRed->value()) / 100.f;
    opts.Green = (float)(ui->sldGreen->value()) / 100.f;
    opts.Blue  = (float)(ui->sldBlue->value()) / 100.f;
    opts.eV    = (float)(pow(2, double(ui->sldEV->value() / 100.)));

    opts.EnableBPC = ui->chkBPC->isChecked();
    opts.EnableSAM = ui->chkSAM->isChecked();

    opts.JpegQuality = ui->spnJpegQty->value();
    opts.JpegSamplingFmt = (fastJpegFormat_t)(ui->cboSamplingFmt->currentData().toInt());

    ui->denoiseCtlr->getDenoiseParams(opts.DenoiseParams, opts.EnableDenoise);
    ui->denoiseCtlr->getStaticDenoiseParams(opts.DenoiseStaticParams);
}

void MainWindow::on_cboBayerPattern_currentIndexChanged(int index)
{
    Q_UNUSED(index);
    if(!mProcessorPtr)
        return;

    mOptions.BayerFormat = fastBayerPattern_t(ui->cboBayerPattern->currentData().toInt());
    raw2Rgb();
}

void MainWindow::on_cboBayerType_currentIndexChanged(int index)
{
    Q_UNUSED(index);
    if(!mProcessorPtr)
        return;

    mOptions.BayerType = fastDebayerType_t(ui->cboBayerType->currentData().toInt());

    //Changing bayer algorythm requires rebuilding CUDA processor
    raw2Rgb(true, true);
}

void MainWindow::on_actionOpenCamera_triggered()
{
#ifdef SUPPORT_XIMEA
    openCamera(0);
#endif
}

void MainWindow::on_actionRecord_toggled(bool arg1)
{
    if(!mProcessorPtr)
        return;

    if(arg1)
    {
        mOptions.Codec = (CUDAProcessorOptions::VideoCodec)(ui->cboOutFormat->currentData().toInt());
        mOptions.JpegQuality = ui->spnJpegQty->value();
        mOptions.JpegSamplingFmt = (fastJpegFormat_t)(ui->cboSamplingFmt->currentData().toInt());

        mProcessorPtr->setOutputPath(ui->txtOutPath->text());
        mProcessorPtr->setFilePrefix(ui->txtFilePrefix->text());
        mProcessorPtr->updateOptions(mOptions);
        mProcessorPtr->startWriting();
    }
    else
    {
        mOptions.Codec = CUDAProcessorOptions::vcNone;
        mProcessorPtr->updateOptions(mOptions);
        mProcessorPtr->stopWriting();
    }
}

void MainWindow::on_actionExit_triggered()
{
    close();
}

void MainWindow::on_actionWB_picker_toggled(bool arg1)
{
    Q_UNUSED(arg1)
}

void MainWindow::onGPUError()
{
    QString strInfo;
    if(!mProcessorPtr)
        return;

    fastStatus_t ret = mProcessorPtr->getLastError();
    if(ret != FAST_OK)
    {
        strInfo = trUtf8("Error occured:\n%1\nCode: %2 (%3)").
                arg(mProcessorPtr->getLastErrorDescription()).
                arg(ret).
                arg(getErrDescription(ret));

        ui->lblInfo->setPlainText(strInfo);
        qDebug() << strInfo;
    }
}

QString MainWindow::getErrDescription(fastStatus_t code)
{
    switch( code )  // !!!!!!!!!!!! some codes are missing !!!!!!!!!!!!!!!!!1
    {
    case FAST_INVALID_DEVICE:
        return QStringLiteral("FAST_INVALID_DEVICE");
    case FAST_INCOMPATIBLE_DEVICE:
        return QStringLiteral("FAST_INCOMPATIBLE_DEVICE");
    case FAST_INSUFFICIENT_DEVICE_MEMORY:
        return QStringLiteral("FAST_INSUFFICIENT_DEVICE_MEMORY");
    case FAST_INSUFFICIENT_HOST_MEMORY:
        return QStringLiteral("FAST_INSUFFICIENT_HOST_MEMORY");
    case FAST_INVALID_HANDLE:
        return QStringLiteral("FAST_INVALID_HANDLE");
    case FAST_INVALID_VALUE:
        return QStringLiteral("FAST_INVALID_VALUE");
    case FAST_UNAPPLICABLE_OPERATION:
        return QStringLiteral("FAST_UNAPPLICABLE_OPERATION");
    case FAST_INVALID_SIZE:
        return QStringLiteral("FAST_INVALID_SIZE");
    case FAST_UNALIGNED_DATA:
        return QStringLiteral("FAST_UNALIGNED_DATA");
    case FAST_INVALID_TABLE:
        return QStringLiteral("FAST_INVALID_TABLE");
    case FAST_BITSTREAM_CORRUPT:
        return QStringLiteral("FAST_BITSTREAM_CORRUPT");
    case FAST_EXECUTION_FAILURE:
        return QStringLiteral("FAST_EXECUTION_FAILURE");
    case FAST_INTERNAL_ERROR:
        return QStringLiteral("FAST_INTERNAL_ERROR");
    case FAST_UNSUPPORTED_SURFACE:
        return QStringLiteral("FAST_UNSUPPORTED_SURFACE");
    case FAST_IO_ERROR:
        return QStringLiteral("FAST_IO_ERROR");
    case FAST_INVALID_FORMAT:
        return QStringLiteral("FAST_INVALID_FORMAT");
    case FAST_UNSUPPORTED_FORMAT:
        return QStringLiteral("FAST_UNSUPPORTED_FORMAT");
    case FAST_MJPEG_THREAD_ERROR:
        return QStringLiteral("FAST_MJPEG_THREAD_ERROR");
    case FAST_MJPEG_OPEN_FILE_ERROR:
        return QStringLiteral("FAST_MJPEG_OPEN_FILE_ERROR");
    case FAST_UNKNOWN_ERROR:
        return QStringLiteral("FAST_UNKNOWN_ERROR");
    default:
        break;
    }
    return QStringLiteral("FAST_OK");
}
void MainWindow::onGPUFinished()
{
    QString strInfo;
    if(!mProcessorPtr)
        return;
    if(!ui->lblInfo->isVisible())
        return;
    fastStatus_t ret = mProcessorPtr->getLastError();
    if(ret != FAST_OK)
    {
        strInfo = trUtf8("Error occured:\n%1\nCode: %2 (%3)").
                arg(mProcessorPtr->getLastErrorDescription()).
                arg(ret).
                arg(getErrDescription(ret));
        ui->lblInfo->setPlainText(strInfo);
        return;
    }

    QMap<QString, float> stats(mProcessorPtr->getStats());


    float val = stats[QStringLiteral("allocatedMem")];
    float viewportMem = stats[QStringLiteral("totalViewportMemory")];

    if(val > 0 && viewportMem > 0)
        val += viewportMem;

    strInfo = trUtf8("Total memory %1 MB, free %2 MB, allocated %3 MB\n").
            arg(double(stats[QStringLiteral("totalMem")] / 1048576), 0, 'f', 0).
            arg(double(stats[QStringLiteral("freeMem")] / 1048576), 0, 'f', 0).
            arg(val > 0 ? double(val) / 1048576 : 0, 0, 'f', 0);

    int w = int(stats[QStringLiteral("inputWidth")]);
    int h = int(stats[QStringLiteral("inputHeight")]);
    if(w > 0 && h > 0)
        strInfo += trUtf8("Input image: %1x%2 pixels\n").arg(w).arg(h);

    val = stats[QStringLiteral("hRawUnpacker")];
    if(val > 0)
        strInfo += trUtf8("Raw Unpacker = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hHostToDeviceAdapter")];
    if(val > 0)
        strInfo += trUtf8("Host-to-device transfer = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hSAM")];
    if(val > 0)
        strInfo += trUtf8("Dark frame and flat field correction = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hLinearizationLut")];
    if(val > 0)
        strInfo += trUtf8("Linearization LUT = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hBpc")];
    if(val > 0)
        strInfo += trUtf8("Bad pixels correction = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hWhiteBalance")];
    if(val > 0)
        strInfo += trUtf8("White balance = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hDebayer")];
    if(val > 0)
        strInfo += trUtf8("Debayer = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hDenoise")];
    if(val > 0)
        strInfo += trUtf8("Denoise = %1 ms\n").arg(double(val), 0, 'f', 2);


    val = stats[QStringLiteral("hOutLut")];
    if(val > 0)
        strInfo += trUtf8("Output gamma = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("h16to8Transform")];
    if(val > 0)
        strInfo += trUtf8("16 to 8 bit transform = %1 ms\n").arg(double(val), 0, 'f', 2);

//    val = stats[QStringLiteral("hHistogram")];
//    if(val > 0)
//        strInfo += trUtf8("Histogram = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hMjpegEncoder")];
    if(val > 0)
        strInfo += trUtf8("JPEG encoder time: %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hDeviceToHostAdapter")];
    if(val > 0)
        strInfo += trUtf8("Device-to-host transfer = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("hExportToDevice")];
    if(val > 0)
        strInfo += trUtf8("Viewport texture copy = %1 ms\n").arg(double(val), 0, 'f', 2);

    val = stats[QStringLiteral("procFrames")];
    if(val >= 0)
        strInfo += trUtf8("Frames written = %1\n").arg(int(val));

    val = stats[QStringLiteral("droppedFrames")];
    if(val >= 0)
        strInfo += trUtf8("Frames dropped = %1\n").arg(int(val));


    float totalGPU = stats[QStringLiteral("totalGPUTime")];
    if(totalGPU > 0)
    {
        totalGPU += stats[QStringLiteral("totalViewportTime")] > 0 ? stats[QStringLiteral("totalViewportTime")] : 0;
        strInfo += trUtf8("Total GPU = %1 ms\n").arg(double(totalGPU), 0, 'f', 2);
    }

    totalGPU = stats[QStringLiteral("totalGPUCPUTime")];
    if(totalGPU > 0)
    {
        totalGPU += stats[QStringLiteral("totalViewportTime")] > 0 ? stats[QStringLiteral("totalViewportTime")] : 0;
        strInfo += trUtf8("\nTotal GPU + CPU = %1 ms\n").arg(double(totalGPU), 0, 'f', 2);
    }

    ui->lblInfo->setPlainText(strInfo);
    ui->lblInfo->moveCursor(QTextCursor::End);

    val = stats[QStringLiteral("acqTime")];
    if(val > 0)
        mFpsLabel->setText( trUtf8("%1 fps").arg(1000000000. / double(val), 0, 'f', 0));

}

void MainWindow::on_chkBPC_toggled(bool checked)
{
    if(!mProcessorPtr)
        return;

    mOptions.EnableBPC = checked;
    mProcessorPtr->updateOptions(mOptions);
    raw2Rgb();
}

void MainWindow::on_btnGetOutPath_clicked()
{
    QString def = ui->txtOutPath->text();
    if(def.isEmpty())
        def = QDir::homePath() + "/FastMV/Record";

    QString str = QFileDialog::getExistingDirectory(this, trUtf8("Choose output path"), def);
    if(str.isNull() || str.isEmpty())
        return;

    QSettings settings;
    settings.setValue("Record/OutPath", str);
    ui->txtOutPath->setText(str);

}

void MainWindow::on_btnGetFPNFile_clicked()
{
    if(!mCameraPtr)
        return;

    QString fileName = QFileDialog::getOpenFileName(this,
                                                    QStringLiteral("Select pgm file for dark frame"),
                                                    QDir::homePath(),
                                                    QStringLiteral("Images (*.pgm)"));

    if(fileName.isEmpty())
        return;

    ui->txtFPNFileName->setText(QDir::toNativeSeparators(fileName));
    mProcessorPtr->setSAM(ui->txtFPNFileName->text(),
                          ui->txtFlatFieldFile->text());
}

void MainWindow::on_btnGetGrayFile_clicked()
{
    if(!mCameraPtr)
        return;

    QString fileName = QFileDialog::getOpenFileName(this,
                                                    QStringLiteral("Select pgm file for flat field"),
                                                    QDir::homePath(),
                                                    QStringLiteral("Images (*.pgm)"));

    if(fileName.isEmpty())
        return;

    ui->txtFlatFieldFile->setText(QDir::toNativeSeparators(fileName));
}

void MainWindow::on_chkSAM_toggled(bool checked)
{
    if(!mProcessorPtr)
        return;

    mOptions.EnableSAM = checked;
    mProcessorPtr->updateOptions(mOptions);
    raw2Rgb();
}

void MainWindow::on_actionOpenBayerPGM_triggered()
{
    openPGMFile();
}

void MainWindow::on_actionOpenGrayPGM_triggered()
{
    openPGMFile(false);
}

void MainWindow::onCameraStateChanged(CameraBase::cmrCameraState newState)
{
    if(newState == CameraBase::cstClosed)
    {
        {
            QSignalBlocker b(ui->actionPlay);
            ui->actionPlay->setChecked(false);
        }
        ui->actionPlay->setEnabled(false);
        ui->actionRecord->setEnabled(false);

    }
    else if(newState == CameraBase::cstStopped)
    {
        ui->actionPlay->setEnabled(true);
        {
            QSignalBlocker b(ui->actionPlay);
            ui->actionPlay->setChecked(false);
        }
        ui->actionRecord->setEnabled(false);
    }
    else if(newState == CameraBase::cstStreaming)
    {
        ui->actionPlay->setEnabled(true);
        {
            QSignalBlocker b(ui->actionPlay);
            ui->actionPlay->setChecked(true);
        }
        ui->actionRecord->setEnabled(true);
    }
}

void MainWindow::on_actionPlay_toggled(bool arg1)
{
    if(!mCameraPtr || !mProcessorPtr)
        return;
    if(arg1)
    {
        updateOptions(mOptions);
        mProcessorPtr->updateOptions(mOptions);
        mProcessorPtr->setSAM(ui->txtFPNFileName->text(),
                              ui->txtFlatFieldFile->text());
        mRendererPtr->showImage();
        mCameraPtr->start();
        mProcessorPtr->start();

    }
    else
    {
        mCameraPtr->stop();
    }
}
