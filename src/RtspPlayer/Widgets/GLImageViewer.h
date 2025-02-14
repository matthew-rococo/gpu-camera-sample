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

#ifndef GLIMAGEVIEWER_H
#define GLIMAGEVIEWER_H

#include <QOpenGLWindow>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QApplication>
#include <QMouseEvent>
#include <QThread>
#include <QWaitCondition>
#include <QMutex>
#include <QSize>
#include <QTimer>
#include <QElapsedTimer>
#include <QEvent>
#if QT_VERSION_MAJOR < 6
#include <QDesktopWidget>
#endif

#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

#include "common.h"
#include "common_utils.h"
#include "SDIConverter.h"

#define EVENT_UPDATE (QEvent::User + 1)
#define EVENT_LOAD_IMAGE (QEvent::User + 2)

class LoadImageEvent : public QEvent
{
public:
    explicit LoadImageEvent():QEvent(static_cast<QEvent::Type>(EVENT_LOAD_IMAGE))
    {}
    LoadImageEvent(PImage img):QEvent(static_cast<QEvent::Type>(EVENT_LOAD_IMAGE)),
            image(img)
    {}

    PImage image;
};

class GLImageViewer;

class GLRenderer : public QObject, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    explicit GLRenderer(QObject *parent = nullptr);
    ~GLRenderer() override;
    QSurfaceFormat format() const { return m_format; }
    QOpenGLContext* context() const {return m_context;}
    void setRenderWnd(GLImageViewer* wnd){mRenderWnd = wnd;}
	void loadImage(PImage image, qint64 bytesReaded);
    void showImage(bool show = true);
    void update();
    QSize imageSize(){return mImageSize;}
    void setImageSize(const QSize& sz){mImageSize = sz;}

    double fps() const {return m_fps;}
    double bytesReaded() const { return m_bytesReaded; }


private slots:
    void render();
    void onTimeout();

protected:
    void customEvent(QEvent* event) override;

private:
    void initialize();
	void loadImageInternal(PImage image);
    bool m_initialized = false;

    QSize  mImageSize;
    GLuint texture = 0;
    GLuint pbo_buffer = 0;

    GLImageViewer* mRenderWnd = nullptr;

    QSurfaceFormat m_format;
    QOpenGLContext *m_context = nullptr;

    QOpenGLShaderProgram* m_program;
    GLint m_texUniform;
    GLuint m_vertPosAttr;
    GLuint m_texPosAttr;

    bool mStreaming;
    bool mShowImage;

    QWaitCondition mCond;
    QMutex mMutex;
    QThread mRenderThread;

	SDIConverter m_sdiConverter;
	void *m_cudaRgb = nullptr;

	int m_prevWidth = 0;
	int m_prevHeight = 0;
    RTSPImage::TYPE m_prevType;

    QTimer m_timer;
	QElapsedTimer m_timeFps;
	qint64 m_wait_timer_ms = 2000;
	qint64 m_frameCount_Fps = 0;
	qint64 m_frameCount = 0;
	qint64 m_LastBytesReaded = 0;
	qint64 m_BytesReaded = 0;
	double m_fps = 0;
	double m_bytesReaded = 0;

	bool initCudaBuffer(PImage image);
	void releaseCudaBuffer();
};

class GLImageViewer : public QOpenGLWindow, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    enum ViewMode
    {
        vmNone,
        vmPan,
        vmZoom,
        vmZoomFit
    };

    enum Tool
    {
        tlNone = 0,
        tlColorPicker,
        tlWBPicker,
        tlRotate
    };

    explicit GLImageViewer(GLRenderer* renderer);
    ~GLImageViewer();

    void     setViewMode(ViewMode);
    ViewMode getViewMode() const;
    void     setZoom(qreal value);
    qreal    getZoom() const;

	void load(PImage image);
    void clear();
    QPointF texTopLeft(){return mTexTopLeft;}

    QPoint screenToBitmap(const QPoint &pt);
    QPoint bitmapToScreen(const QPoint &pt);

    Tool getCurrentTool() const{return currentTool;}
    void setCurrentTool(const Tool &tool);

    //Patch for high dpi display
#if QT_VERSION_MAJOR >= 6
    inline int width() const { return QOpenGLWindow::geometry().width() * screen()->devicePixelRatio(); }
    inline int height() const { return QOpenGLWindow::geometry().height() * screen()->devicePixelRatio(); }
    QSize size() const  override { return QOpenGLWindow::geometry().size() * screen()->devicePixelRatio(); }
    QRect geometry()const { return { QOpenGLWindow::geometry().topLeft() * screen()->devicePixelRatio(),
                QOpenGLWindow::geometry().size() * screen()->devicePixelRatio()}; }
#else
    inline int width() const { return QOpenGLWindow::geometry().width() * QApplication::desktop()->devicePixelRatio(); }
    inline int height() const { return QOpenGLWindow::geometry().height() * QApplication::desktop()->devicePixelRatio(); }
    QSize size() const  override { return QOpenGLWindow::geometry().size() * QApplication::desktop()->devicePixelRatio(); }
    QRect geometry()const { return { QOpenGLWindow::geometry().topLeft() * QApplication::desktop()->devicePixelRatio(),
                QOpenGLWindow::geometry().size() * QApplication::desktop()->devicePixelRatio()}; }
#endif

protected:
    void exposeEvent(QExposeEvent *event) Q_DECL_OVERRIDE;
    void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;

    virtual void resizeEvent(QResizeEvent * event);
    virtual void mouseMoveEvent(QMouseEvent * event);
    virtual void mousePressEvent(QMouseEvent * event);
    virtual void mouseReleaseEvent(QMouseEvent * event);

signals:
    void zoomChanged(qreal);
    void sizeChanged(QSize& newSize);
    void contextMenu(QPoint pt);
    void mouseClicked(QMouseEvent* event);
    void newWBFromPoint(const QPoint& pt);

public slots:

private:
    void setFitZoom(QSize szClient);
    void adjustTexTopLeft();
    void render();
    void setZoomInternal(qreal newZoom, QPoint fixPoint = QPoint());
    void update()
    {
        if(isExposed() && mRenderer)
            mRenderer->update();
    }

    ViewMode    mViewMode = vmNone;
    Tool        currentTool = tlNone;
    bool        mShowImage = true;
    qreal       mZoom = 1.f;
    QSize       mSzHint;
    QPoint      mPtDown;
    QPointF     mTexTopLeft;
    GLRenderer* mRenderer = nullptr;
};

#endif // GLIMAGEVIEWER_H
