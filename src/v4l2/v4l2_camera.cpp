/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019, Google Inc.
 *
 * v4l2_camera.cpp - V4L2 compatibility camera
 */

#include "v4l2_camera.h"

#include <errno.h>

#include "log.h"
#include "utils.h"

using namespace libcamera;

LOG_DECLARE_CATEGORY(V4L2Compat);

V4L2FrameMetadata::V4L2FrameMetadata(Buffer *buffer)
	: index_(buffer->index()), bytesused_(buffer->bytesused()),
	  timestamp_(buffer->timestamp()), sequence_(buffer->sequence()),
	  status_(buffer->status())
{
}

V4L2Camera::V4L2Camera(std::shared_ptr<Camera> camera)
	: camera_(camera), isRunning_(false)
{
	camera_->requestCompleted.connect(this, &V4L2Camera::requestComplete);
}

V4L2Camera::~V4L2Camera()
{
	camera_->release();
}

int V4L2Camera::open()
{
	/* \todo Support multiple open. */
	if (camera_->acquire() < 0) {
		LOG(V4L2Compat, Error) << "Failed to acquire camera";
		return -EINVAL;
	}

	config_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
	if (!config_) {
		camera_->release();
		return -EINVAL;
	}

	return 0;
}

void V4L2Camera::close()
{
	camera_->release();
}

void V4L2Camera::getStreamConfig(StreamConfiguration *streamConfig)
{
	*streamConfig = config_->at(0);
}

std::vector<V4L2FrameMetadata> V4L2Camera::completedBuffers()
{
	std::vector<V4L2FrameMetadata> v;

	bufferLock_.lock();
	for (std::unique_ptr<V4L2FrameMetadata> &metadata : completedBuffers_)
		v.push_back(*metadata.get());
	completedBuffers_.clear();
	bufferLock_.unlock();

	return v;
}

void V4L2Camera::requestComplete(Request *request)
{
	if (request->status() == Request::RequestCancelled)
		return;

	/* We only have one stream at the moment. */
	bufferLock_.lock();
	Buffer *buffer = request->buffers().begin()->second;
	std::unique_ptr<V4L2FrameMetadata> metadata =
		utils::make_unique<V4L2FrameMetadata>(buffer);
	completedBuffers_.push_back(std::move(metadata));
	bufferLock_.unlock();

	bufferSema_.release();
}

int V4L2Camera::configure(StreamConfiguration *streamConfigOut,
			  const Size &size, PixelFormat pixelformat,
			  unsigned int bufferCount)
{
	StreamConfiguration &streamConfig = config_->at(0);
	streamConfig.size.width = size.width;
	streamConfig.size.height = size.height;
	streamConfig.pixelFormat = pixelformat;
	streamConfig.bufferCount = bufferCount;
	/* \todo memoryType (interval vs external) */

	CameraConfiguration::Status validation = config_->validate();
	if (validation == CameraConfiguration::Invalid) {
		LOG(V4L2Compat, Debug) << "Configuration invalid";
		return -EINVAL;
	}
	if (validation == CameraConfiguration::Adjusted)
		LOG(V4L2Compat, Debug) << "Configuration adjusted";

	LOG(V4L2Compat, Debug) << "Validated configuration is: "
			      << streamConfig.toString();

	int ret = camera_->configure(config_.get());
	if (ret < 0)
		return ret;

	*streamConfigOut = config_->at(0);

	return 0;
}

int V4L2Camera::allocBuffers(unsigned int count)
{
	int ret = camera_->allocateBuffers();
	return ret == -EACCES ? -EBUSY : ret;
}

void V4L2Camera::freeBuffers()
{
	camera_->freeBuffers();
}

FileDescriptor V4L2Camera::getBufferFd(unsigned int index)
{
	Stream *stream = *camera_->streams().begin();
	return stream->buffers()[index].planes()[0].fd;
}

int V4L2Camera::streamOn()
{
	if (isRunning_)
		return 0;

	int ret = camera_->start();
	if (ret < 0)
		return ret == -EACCES ? -EBUSY : ret;

	isRunning_ = true;

	for (std::unique_ptr<Request> &req : pendingRequests_) {
		/* \todo What should we do if this returns -EINVAL? */
		ret = camera_->queueRequest(req.release());
		if (ret < 0)
			return ret == -EACCES ? -EBUSY : ret;
	}

	pendingRequests_.clear();

	return 0;
}

int V4L2Camera::streamOff()
{
	/* \todo Restore buffers to reqbufs state? */
	if (!isRunning_)
		return 0;

	int ret = camera_->stop();
	if (ret < 0)
		return ret == -EACCES ? -EBUSY : ret;

	isRunning_ = false;

	return 0;
}

int V4L2Camera::qbuf(unsigned int index)
{
	Stream *stream = config_->at(0).stream();
	std::unique_ptr<Buffer> buffer = stream->createBuffer(index);
	if (!buffer) {
		LOG(V4L2Compat, Error) << "Can't create buffer";
		return -ENOMEM;
	}

	std::unique_ptr<Request> request =
		std::unique_ptr<Request>(camera_->createRequest());
	if (!request) {
		LOG(V4L2Compat, Error) << "Can't create request";
		return -ENOMEM;
	}

	int ret = request->addBuffer(stream, std::move(buffer));
	if (ret < 0) {
		LOG(V4L2Compat, Error) << "Can't set buffer for request";
		return -ENOMEM;
	}

	if (!isRunning_) {
		pendingRequests_.push_back(std::move(request));
		return 0;
	}

	ret = camera_->queueRequest(request.release());
	if (ret < 0) {
		LOG(V4L2Compat, Error) << "Can't queue request";
		return ret == -EACCES ? -EBUSY : ret;
	}

	return 0;
}
