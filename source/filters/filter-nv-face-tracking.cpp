/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2017 Michael Fabian Dirks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "filter-nv-face-tracking.hpp"
#include <algorithm>
#include <filesystem>
#include <util/platform.h>
#include "nvidia/cuda/nvidia-cuda-context-stack.hpp"
#include "obs/gs/gs-helper.hpp"
#include "obs/obs-tools.hpp"
#include "utility.hpp"

#define ST "Filter.Nvidia.FaceTracking"
#define ST_ROI "Filter.Nvidia.FaceTracking.ROI"
#define ST_ROI_ZOOM "Filter.Nvidia.FaceTracking.ROI.Zoom"
#define SK_ROI_ZOOM "ROI.Zoom"
#define ST_ROI_OFFSET "Filter.Nvidia.FaceTracking.ROI.Offset"
#define ST_ROI_OFFSET_X "Filter.Nvidia.FaceTracking.ROI.Offset.X"
#define SK_ROI_OFFSET_X "ROI.Offset.X"
#define ST_ROI_OFFSET_Y "Filter.Nvidia.FaceTracking.ROI.Offset.Y"
#define SK_ROI_OFFSET_Y "ROI.Offset.Y"
#define ST_ROI_STABILITY "Filter.Nvidia.FaceTracking.ROI.Stability"
#define SK_ROI_STABILITY "ROI.Stability"

using namespace streamfx::filter::nvidia;

void ar_feature_deleter(NvAR_FeatureHandle v)
{
	face_tracking_factory::get()->get_ar()->destroy(v);
}

face_tracking_instance::face_tracking_instance(obs_data_t* settings, obs_source_t* self)
	: obs::source_instance(settings, self),

	  _rt_is_fresh(false), _rt(),

	  _cfg_roi_zoom(1.0), _cfg_roi_offset({0., 0.}), _cfg_roi_stability(1.0),

	  _roi_center(), _roi_size(), _roi_geom(),

	  _cuda(face_tracking_factory::get()->get_cuda()), _cuda_ctx(face_tracking_factory::get()->get_cuda_context()),
	  _cuda_stream(),

	  _ar_library(face_tracking_factory::get()->get_ar()), _ar_loaded(false), _ar_feature(), _ar_tracked(true),
	  _ar_bboxes_data(), _ar_bboxes(), _ar_bboxes_confidence(),

	  _ar_texture(), _ar_texture_cuda_fresh(false), _ar_texture_cuda(), _ar_texture_cuda_mem(), _ar_image(),
	  _ar_image_bgr(), _ar_image_temp()
{
	// Create Graphics resources for everything.
	{
		auto gctx = gs::context{};
		_rt       = std::make_shared<gs::rendertarget>(GS_RGBA, GS_ZS_NONE);
		_roi_geom = std::make_shared<gs::vertex_buffer>(4, 1);
	}

	// Initialize everything.
	{
		auto         cctx = std::make_shared<::nvidia::cuda::context_stack>(_cuda, _cuda_ctx);
		std::int32_t minPrio, maxPrio;
		_cuda->cuCtxGetStreamPriorityRange(&minPrio, &maxPrio);
		_cuda_stream = std::make_shared<::nvidia::cuda::stream>(_cuda, ::nvidia::cuda::cu_stream_flags::NON_BLOCKING,
																minPrio + ((maxPrio - minPrio) / 2));
	}

#ifdef ENABLE_PROFILING
	// Profiling
	_profile_capture         = util::profiler::create();
	_profile_capture_realloc = util::profiler::create();
	_profile_capture_copy    = util::profiler::create();
	_profile_ar_realloc      = util::profiler::create();
	_profile_ar_copy         = util::profiler::create();
	_profile_ar_transfer     = util::profiler::create();
	_profile_ar_run          = util::profiler::create();
	_profile_ar_calc         = util::profiler::create();
#endif

	// Asynchronously load Face Tracking.
	async_initialize(nullptr);
}

face_tracking_instance::~face_tracking_instance()
{
	_ar_library->image_dealloc(&_ar_image_temp);
	_ar_library->image_dealloc(&_ar_image_bgr);
}

void face_tracking_instance::async_initialize(std::shared_ptr<void> ptr)
{
	struct async_data {
		std::shared_ptr<obs_weak_source_t> source;
		std::string                        models_path;
	};

	if (!ptr) {
		// Spawn the work for the threadpool.
		std::shared_ptr<async_data> data = std::make_shared<async_data>();
		data->source =
			std::shared_ptr<obs_weak_source_t>(obs_source_get_weak_source(_self), obs::obs_weak_source_deleter);

		std::filesystem::path models_path = _ar_library->get_ar_sdk_path();
		models_path                       = models_path.append("models");
		models_path                       = std::filesystem::absolute(models_path);
		models_path.concat("\\");
		data->models_path = models_path.string();

		streamfx::threadpool()->push(std::bind(&face_tracking_instance::async_initialize, this, std::placeholders::_1),
									 data);
	} else {
		std::shared_ptr<async_data> data = std::static_pointer_cast<async_data>(ptr);

		// Try and acquire a strong source reference.
		std::shared_ptr<obs_source_t> ref =
			std::shared_ptr<obs_source_t>(obs_weak_source_get_source(data->source.get()), obs::obs_source_deleter);
		if (!ref) { // If that failed, the source we are working for was deleted - abort now.
			return;
		}

		// Update the current CUDA context for working.
		gs::context gctx;
		auto        cctx = std::make_shared<::nvidia::cuda::context_stack>(_cuda, _cuda_ctx);

		// Create Face Detection feature.
		{
			NvAR_FeatureHandle fd_inst;
			if (NvCV_Status res = _ar_library->create(NvAR_Feature_FaceDetection, &fd_inst); res != NVCV_SUCCESS) {
				throw std::runtime_error("Failed to create Face Detection feature.");
			}
			_ar_feature = std::shared_ptr<nvAR_Feature>{fd_inst, ar_feature_deleter};
		}

		// Set the correct CUDA stream for processing.
		if (NvCV_Status res = _ar_library->set_cuda_stream(_ar_feature.get(), NvAR_Parameter_Config(CUDAStream),
														   reinterpret_cast<CUstream>(_cuda_stream->get()));
			res != NVCV_SUCCESS) {
			throw std::runtime_error("Failed to set CUDA stream.");
		}

		// Set the correct models path.
		if (NvCV_Status res =
				_ar_library->set_string(_ar_feature.get(), NvAR_Parameter_Config(ModelDir), data->models_path.c_str());
			res != NVCV_SUCCESS) {
			throw std::runtime_error("Unable to set model path.");
		}

		// Finally enable Temporal tracking if possible.
		if (NvCV_Status res = _ar_library->set_uint32(_ar_feature.get(), NvAR_Parameter_Config(Temporal), 1);
			res != NVCV_SUCCESS) {
			LOG_WARNING("<%s> Unable to enable Temporal tracking mode.", obs_source_get_name(ref.get()));
		}

		// Create Bounding Boxes Data
		_ar_bboxes_data.assign(1, {0., 0., 0., 0.});
		_ar_bboxes.boxes     = _ar_bboxes_data.data();
		_ar_bboxes.max_boxes = std::clamp<std::uint8_t>(static_cast<std::uint8_t>(_ar_bboxes_data.size()), 0, 255);
		_ar_bboxes.num_boxes = 0;
		_ar_bboxes_confidence.resize(_ar_bboxes_data.size());
		if (NvCV_Status res = _ar_library->set_object(_ar_feature.get(), NvAR_Parameter_Output(BoundingBoxes),
													  &_ar_bboxes, sizeof(NvAR_BBoxes));
			res != NVCV_SUCCESS) {
			throw std::runtime_error("Failed to set BoundingBoxes for Face Tracking feature.");
		}
		if (NvCV_Status res = _ar_library->set_float32_array(
				_ar_feature.get(), NvAR_Parameter_Output(BoundingBoxesConfidence), _ar_bboxes_confidence.data(),
				static_cast<int>(_ar_bboxes_confidence.size()));
			res != NVCV_SUCCESS) {
			throw std::runtime_error("Failed to set BoundingBoxesConfidence for Face Tracking feature.");
		}

		// And finally, load the feature (takes long).
		if (NvCV_Status res = _ar_library->load(_ar_feature.get()); res != NVCV_SUCCESS) {
			LOG_ERROR("<%s> Failed to load Face Tracking feature.", obs_source_get_name(_self));
			_ar_loaded = false;
			return;
		} else {
			_ar_loaded = true;
		}
	}
}

void face_tracking_instance::refresh_geometry()
{ // Update Region of Interest Geometry.
	std::unique_lock<std::mutex> lock(_roi_lock);

	auto v0 = _roi_geom->at(0);
	auto v1 = _roi_geom->at(1);
	auto v2 = _roi_geom->at(2);
	auto v3 = _roi_geom->at(3);

	*v0.color = 0xFFFFFFFF;
	*v1.color = 0xFFFFFFFF;
	*v2.color = 0xFFFFFFFF;
	*v3.color = 0xFFFFFFFF;

	vec3_set(v3.position, static_cast<float_t>(_size.first), static_cast<float_t>(_size.second), 0.);
	vec3_set(v2.position, v3.position->x, 0., 0.);
	vec3_set(v1.position, 0., v3.position->y, 0.);
	vec3_set(v0.position, 0., 0., 0.);

	vec4_set(v0.uv[0],
			 static_cast<float_t>((_roi_center.first - _roi_size.first / 2.) / static_cast<double_t>(_size.first)),
			 static_cast<float_t>((_roi_center.second - _roi_size.second / 2.) / static_cast<double_t>(_size.second)),
			 0., 0.);
	vec4_set(v1.uv[0],
			 static_cast<float_t>((_roi_center.first - _roi_size.first / 2.) / static_cast<double_t>(_size.first)),
			 static_cast<float_t>((_roi_center.second + _roi_size.second / 2.) / static_cast<double_t>(_size.second)),
			 0., 0.);
	vec4_set(v2.uv[0],
			 static_cast<float_t>((_roi_center.first + _roi_size.first / 2.) / static_cast<double_t>(_size.first)),
			 static_cast<float_t>((_roi_center.second - _roi_size.second / 2.) / static_cast<double_t>(_size.second)),
			 0., 0.);
	vec4_set(v3.uv[0],
			 static_cast<float_t>((_roi_center.first + _roi_size.first / 2.) / static_cast<double_t>(_size.first)),
			 static_cast<float_t>((_roi_center.second + _roi_size.second / 2.) / static_cast<double_t>(_size.second)),
			 0., 0.);

	_roi_geom->update();
}

void face_tracking_instance::async_track(std::shared_ptr<void> ptr)
{
	struct async_data {
		std::shared_ptr<obs_weak_source_t> source;
	};

	if (!ptr) {
		// Spawn the work for the threadpool.
		std::shared_ptr<async_data> data = std::make_shared<async_data>();
		data->source =
			std::shared_ptr<obs_weak_source_t>(obs_source_get_weak_source(_self), obs::obs_weak_source_deleter);

		// Check if things exist as planned.
		if (!_ar_texture || (_ar_texture->get_width() != _size.first) || (_ar_texture->get_height() != _size.second)) {
#ifdef ENABLE_PROFILING
			auto             prof = _profile_capture_realloc->track();
			gs::debug_marker marker{gs::debug_color_allocate, "Reallocate GPU Buffer"};
#endif

			_ar_texture =
				std::make_shared<gs::texture>(_size.first, _size.second, GS_RGBA, 1, nullptr, gs::texture::flags::None);
			_ar_texture_cuda_fresh = false;
		}

		{ // Copy texture
#ifdef ENABLE_PROFILING
			auto             prof = _profile_capture_copy->track();
			gs::debug_marker marker{gs::debug_color_copy, "Copy Capture", obs_source_get_name(_self)};
#endif

			gs_copy_texture(_ar_texture->get_object(), _rt->get_texture()->get_object());
		}

		// Push work
		streamfx::threadpool()->push(std::bind(&face_tracking_instance::async_track, this, std::placeholders::_1),
									 data);
	} else {
		std::shared_ptr<async_data> data = std::static_pointer_cast<async_data>(ptr);

		// Try and acquire a strong source reference.
		std::shared_ptr<obs_source_t> ref =
			std::shared_ptr<obs_source_t>(obs_weak_source_get_source(data->source.get()), obs::obs_source_deleter);
		if (!ref) { // If that failed, the source we are working for was deleted - abort now.
			return;
		}

		// Acquire GS context.
		gs::context gctx;

		// Update the current CUDA context for working.
		auto cctx = std::make_shared<::nvidia::cuda::context_stack>(_cuda, _cuda_ctx);

		// Refresh any now broken buffers.
		if (!_ar_texture_cuda_fresh) {
#ifdef ENABLE_PROFILING
			auto             prof = _profile_ar_realloc->track();
			gs::debug_marker marker{gs::debug_color_allocate, "%s: Reallocate CUDA Buffers",
									obs_source_get_name(_self)};
#endif

			// Assign new texture and allocate new memory.
			std::size_t pitch    = _size.first * 4ul;
			_ar_texture_cuda     = std::make_shared<::nvidia::cuda::gstexture>(_cuda, _ar_texture);
			_ar_texture_cuda_mem = std::make_shared<::nvidia::cuda::memory>(_cuda, pitch * _size.second);
			_ar_library->image_init(&_ar_image, static_cast<unsigned int>(_size.first),
									static_cast<unsigned int>(_size.second), static_cast<int>(pitch),
									reinterpret_cast<void*>(_ar_texture_cuda_mem->get()), NVCV_RGBA, NVCV_U8,
									NVCV_INTERLEAVED, NVCV_CUDA);

			// Reallocate transposed buffer.
			_ar_library->image_dealloc(&_ar_image_bgr);
			_ar_library->image_alloc(&_ar_image_bgr, static_cast<unsigned int>(_size.first),
									 static_cast<unsigned int>(_size.second), NVCV_BGR, NVCV_U8, NVCV_INTERLEAVED,
									 NVCV_CUDA, 0);
			_ar_library->image_dealloc(&_ar_image_temp);

			// Finally set the input object.
			if (NvCV_Status res = _ar_library->set_object(_ar_feature.get(), NvAR_Parameter_Input(Image),
														  &_ar_image_bgr, sizeof(NvCVImage));
				res != NVCV_SUCCESS) {
				LOG_ERROR("<%s> Failed to update input image for tracking.", obs_source_get_name(_self));
				return;
			}

			// And mark the new texture as fresh.
			_ar_texture_cuda_fresh = true;
		}

		{ // Copy from CUDA array to CUDA device memory.
#ifdef ENABLE_PROFILING
			auto prof = _profile_ar_copy->track();
#endif

			::nvidia::cuda::cu_memcpy2d_t mc;
			mc.src_x_in_bytes  = 0;
			mc.src_y           = 0;
			mc.src_memory_type = ::nvidia::cuda::cu_memory_type::ARRAY;
			mc.src_host        = nullptr;
			mc.src_device      = 0;
			mc.src_array       = _ar_texture_cuda->map(_cuda_stream);
			mc.src_pitch       = static_cast<size_t>(_ar_image.pitch);
			mc.dst_x_in_bytes  = 0;
			mc.dst_y           = 0;
			mc.dst_memory_type = ::nvidia::cuda::cu_memory_type::DEVICE;
			mc.dst_host        = 0;
			mc.dst_device      = reinterpret_cast<::nvidia::cuda::cu_device_ptr_t>(_ar_image.pixels);
			mc.dst_array       = 0;
			mc.dst_pitch       = static_cast<size_t>(_ar_image.pitch);
			mc.width_in_bytes  = static_cast<size_t>(_ar_image.pitch);
			mc.height          = _ar_image.height;

			if (::nvidia::cuda::cu_result res = _cuda->cuMemcpy2D(&mc); res != ::nvidia::cuda::cu_result::SUCCESS) {
				LOG_ERROR("<%s> Failed to prepare buffers for tracking.", obs_source_get_name(_self));
				return;
			}
		}

		{ // Convert from RGBA 32-bit to BGR 24-bit.
#ifdef ENABLE_PROFILING
			auto prof = _profile_ar_transfer->track();
#endif

			if (NvCV_Status res =
					_ar_library->image_transfer(&_ar_image, &_ar_image_bgr, 1.0,
												reinterpret_cast<CUstream_st*>(_cuda_stream->get()), &_ar_image_temp);
				res != NVCV_SUCCESS) {
				LOG_ERROR("<%s> Failed to convert from RGBX 32-bit to BGR 24-bit.", obs_source_get_name(_self));
				return;
			}
		}

		{ // Track any faces.
#ifdef ENABLE_PROFILING
			auto prof = _profile_ar_run->track();
#endif

			if (NvCV_Status res = _ar_library->run(_ar_feature.get()); res != NVCV_SUCCESS) {
				LOG_ERROR("<%s> Failed to run tracking.", obs_source_get_name(_self));
				return;
			}
		}

		if ((_ar_bboxes.num_boxes == 0) || (_ar_bboxes_confidence.at(0) < 0.5)) {
			// Not confident enough or not tracking anything, return to full frame after a bit.
		} else {
#ifdef ENABLE_PROFILING
			auto prof = _profile_ar_calc->track();
#endif

			double_t aspect = double_t(_size.first) / double_t(_size.second);

			// Store values and center.
			double_t bbox_w  = _ar_bboxes.boxes[0].width;
			double_t bbox_h  = _ar_bboxes.boxes[0].height;
			double_t bbox_cx = _ar_bboxes.boxes[0].x + bbox_w / 2.0;
			double_t bbox_cy = _ar_bboxes.boxes[0].y + bbox_h / 2.0;

			// Zoom, Aspect Ratio, Offset
			bbox_h = util::math::lerp<double_t>(_size.second, bbox_h, _cfg_roi_zoom);
			bbox_h = std::clamp(bbox_h, 10 * aspect, static_cast<double_t>(_size.second));
			bbox_w = bbox_h * aspect;
			bbox_cx += _ar_bboxes.boxes[0].width * _cfg_roi_offset.first;
			bbox_cy += _ar_bboxes.boxes[0].height * _cfg_roi_offset.second;

			// Fit back into the frame
			// - Above code guarantees that height is never bigger than the height of the frame.
			// - Which also guarantees that width is never bigger than the width of the frame.
			// Only cx and cy need to be adjusted now to always be in the frame.
			bbox_cx = std::clamp(bbox_cx, (bbox_w / 2.), static_cast<double_t>(_size.first) - (bbox_w / 2.));
			bbox_cy = std::clamp(bbox_cy, (bbox_h / 2.), static_cast<double_t>(_size.second) - (bbox_h / 2.));

			// Filter values
			auto size_w   = _roi_filters[2].filter(bbox_w);
			auto size_h   = _roi_filters[3].filter(bbox_h);
			auto center_x = _roi_filters[0].filter(bbox_cx);
			auto center_y = _roi_filters[1].filter(bbox_cy);

			// Fix NaN/Infinity
			if (std::isfinite(size_w) && std::isfinite(size_h) && std::isfinite(center_x) && std::isfinite(center_y)) {
				std::unique_lock<std::mutex> lock(_roi_lock);
				_roi_center.first  = center_x;
				_roi_center.second = center_y;
				_roi_size.first    = size_w;
				_roi_size.second   = size_h;
			} else {
				std::unique_lock<std::mutex> lock(_roi_lock);
				roi_refresh();
			}
		}

		_ar_tracked = true;
	}
}

void face_tracking_instance::roi_refresh()
{
	double_t kalman_q = util::math::lerp<double_t>(1.0, 1e-6, _cfg_roi_stability);
	double_t kalman_r = util::math::lerp<double_t>(std::numeric_limits<double_t>::epsilon(), 1e+2, _cfg_roi_stability);

	_roi_filters[0] = util::math::kalman1D<double_t>{kalman_q, kalman_r, 1.0, _roi_center.first};
	_roi_filters[1] = util::math::kalman1D<double_t>{kalman_q, kalman_r, 1.0, _roi_center.second};
	_roi_filters[2] = util::math::kalman1D<double_t>{kalman_q, kalman_r, 1.0, _roi_size.first};
	_roi_filters[3] = util::math::kalman1D<double_t>{kalman_q, kalman_r, 1.0, _roi_size.second};
}

void face_tracking_instance::roi_reset()
{
	_roi_center.first  = static_cast<double_t>(_size.first / 2);
	_roi_center.second = static_cast<double_t>(_size.second / 2);
	_roi_size.first    = static_cast<double_t>(_size.first);
	_roi_size.second   = static_cast<double_t>(_size.second);

	roi_refresh();
}

void face_tracking_instance::load(obs_data_t* data)
{
	update(data);
}

void face_tracking_instance::migrate(obs_data_t* data, std::uint64_t version) {}

void face_tracking_instance::update(obs_data_t* data)
{
	_cfg_roi_zoom          = obs_data_get_double(data, SK_ROI_ZOOM) / 100.0;
	_cfg_roi_offset.first  = obs_data_get_double(data, SK_ROI_OFFSET_X) / 100.0;
	_cfg_roi_offset.second = obs_data_get_double(data, SK_ROI_OFFSET_Y) / 100.0;
	_cfg_roi_stability     = obs_data_get_double(data, SK_ROI_STABILITY) / 100.0;

	// Refresh the Region Of Interest
	std::unique_lock<std::mutex> lock(_roi_lock);
	roi_refresh();
}

void face_tracking_instance::video_tick(float_t seconds)
{
	// If we aren't yet ready to do work, abort for now.
	if (!_ar_loaded) {
		return;
	}

	if (obs_source_t* target = obs_filter_get_target(_self); target != nullptr) {
		_size.first  = obs_source_get_width(target);
		_size.second = obs_source_get_height(target);
	}

	_rt_is_fresh = false;
}

void face_tracking_instance::video_render(gs_effect_t* effect)
{
	obs_source_t* filter_parent  = obs_filter_get_parent(_self);
	obs_source_t* filter_target  = obs_filter_get_target(_self);
	gs_effect_t*  default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	if (!filter_parent || !filter_target || !_size.first || !_size.second || !_ar_loaded) {
		obs_source_skip_video_filter(_self);
		return;
	}

#ifdef ENABLE_PROFILING
	gs::debug_marker gdmp{gs::debug_color_source, "Nvidia Face Tracking '%s' on '%s'", obs_source_get_name(_self),
						  obs_source_get_name(obs_filter_get_parent(_self))};
#endif

	if (!_rt_is_fresh) { // Capture the filter stack "below" us.
#ifdef ENABLE_PROFILING
		auto prof = _profile_capture->track();
#endif

		{
#ifdef ENABLE_PROFILING
			gs::debug_marker gdm{gs::debug_color_cache, "Cache"};
#endif

			if (obs_source_process_filter_begin(_self, _rt->get_color_format(), OBS_ALLOW_DIRECT_RENDERING)) {
				auto op  = _rt->render(_size.first, _size.second);
				vec4 clr = {0., 0., 0., 0.};

				gs_ortho(0., static_cast<float_t>(_size.first), 0., static_cast<float_t>(_size.second), 0., 1.);
				gs_clear(GS_CLEAR_COLOR, &clr, 0., 0.);

				obs_source_process_filter_tech_end(_self, default_effect, _size.first, _size.second, "Draw");
			} else {
				obs_source_skip_video_filter(_self);
				return;
			}
		}

		if (_ar_tracked) {
#ifdef ENABLE_PROFILING
			gs::debug_marker gdm{gs::debug_color_convert, "Trigger Async Tracking Task"};
#endif

			async_track(nullptr);
			refresh_geometry();
		}

		_rt_is_fresh = true;
	}

	{ // Draw Texture
#ifdef ENABLE_PROFILING
		gs::debug_marker gdm{gs::debug_color_render, "Render"};
#endif

		gs_effect_set_texture(gs_effect_get_param_by_name(effect ? effect : default_effect, "image"),
							  _rt->get_texture()->get_object());
		gs_load_vertexbuffer(_roi_geom->update());
		while (gs_effect_loop(effect ? effect : default_effect, "Draw")) {
			gs_draw(gs_draw_mode::GS_TRISTRIP, 0, _roi_geom->size());
		}
		gs_load_vertexbuffer(nullptr);
	}
}

#ifdef ENABLE_PROFILING
bool face_tracking_instance::button_profile(obs_properties_t* props, obs_property_t* property)
{
	LOG_INFO("%-22s: %-10s %-10s %-10s %-10s %-10s", "Task", "Total", "Count", "Average", "99.9%ile", "95.0%ile");

	std::pair<std::string, std::shared_ptr<util::profiler>> profilers[]{
		{"Capture", _profile_capture},   {"Reallocate", _profile_capture_realloc},
		{"Copy", _profile_capture_copy}, {"AR Reallocate", _profile_ar_realloc},
		{"AR Copy", _profile_ar_copy},   {"AR Convert", _profile_ar_transfer},
		{"AR Run", _profile_ar_run},     {"AR Calculate", _profile_ar_calc},
	};
	for (auto& kv : profilers) {
		LOG_INFO("  %-20s: %8lldµs %10lld %8lldµs %8lldµs %8lldµs", kv.first.c_str(),
				 std::chrono::duration_cast<std::chrono::microseconds>(kv.second->total_duration()).count(),
				 kv.second->count(), static_cast<std::int64_t>(kv.second->average_duration() / 1000.0),
				 std::chrono::duration_cast<std::chrono::microseconds>(kv.second->percentile(0.999)).count(),
				 std::chrono::duration_cast<std::chrono::microseconds>(kv.second->percentile(0.95)).count());
	}

	return false;
}
#endif

face_tracking_factory::face_tracking_factory()
{
	// Try and load CUDA.
	_cuda = std::make_shared<::nvidia::cuda::cuda>();

	// Try and load AR.
	_ar = std::make_shared<::nvidia::ar::ar>();

	// Initialize CUDA
	{
		auto gctx = gs::context{};
#ifdef WIN32
		if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
			_cuda_ctx =
				std::make_shared<::nvidia::cuda::context>(_cuda, reinterpret_cast<ID3D11Device*>(gs_get_device_obj()));
		}
#endif
		if (gs_get_device_type() == GS_DEVICE_OPENGL) {
			throw std::runtime_error("OpenGL not supported.");
		}
	}

	// Info
	_info.id           = "streamfx-nvidia-face-tracking";
	_info.type         = OBS_SOURCE_TYPE_FILTER;
	_info.output_flags = OBS_SOURCE_VIDEO;

	set_resolution_enabled(false);
	finish_setup();
}

face_tracking_factory::~face_tracking_factory() {}

const char* face_tracking_factory::get_name()
{
	return D_TRANSLATE(ST);
}

void face_tracking_factory::get_defaults2(obs_data_t* data)
{
	obs_data_set_default_double(data, SK_ROI_ZOOM, 50.0);
	obs_data_set_default_double(data, SK_ROI_OFFSET_X, 0.0);
	obs_data_set_default_double(data, SK_ROI_OFFSET_Y, -15.0);
	obs_data_set_default_double(data, SK_ROI_STABILITY, 50.0);
}

obs_properties_t* face_tracking_factory::get_properties2(face_tracking_instance* data)
{
	obs_properties_t* pr = obs_properties_create();

	{
		auto grp = obs_properties_create();
		obs_properties_add_group(pr, ST_ROI, D_TRANSLATE(ST_ROI), OBS_GROUP_NORMAL, grp);
		{
			auto p =
				obs_properties_add_float_slider(grp, SK_ROI_STABILITY, D_TRANSLATE(ST_ROI_STABILITY), 0, 100.0, 0.01);
			obs_property_set_long_description(p, D_TRANSLATE(D_DESC(ST_ROI_STABILITY)));
			obs_property_float_set_suffix(p, " %");
		}
		{
			auto p = obs_properties_add_float_slider(grp, SK_ROI_ZOOM, D_TRANSLATE(ST_ROI_ZOOM), 0, 200.0, 0.01);
			obs_property_set_long_description(p, D_TRANSLATE(D_DESC(ST_ROI_ZOOM)));
			obs_property_float_set_suffix(p, " %");
		}
		{
			auto grp2 = obs_properties_create();
			obs_properties_add_group(grp, ST_ROI_OFFSET, D_TRANSLATE(ST_ROI_OFFSET), OBS_GROUP_NORMAL, grp2);

			{
				auto p = obs_properties_add_float_slider(grp2, SK_ROI_OFFSET_X, D_TRANSLATE(ST_ROI_OFFSET_X), -50.0,
														 50.0, 0.01);
				obs_property_set_long_description(p, D_TRANSLATE(D_DESC(ST_ROI_OFFSET_X)));
				obs_property_float_set_suffix(p, " %");
			}
			{
				auto p = obs_properties_add_float_slider(grp2, SK_ROI_OFFSET_Y, D_TRANSLATE(ST_ROI_OFFSET_Y), -50.0,
														 50.0, 0.01);
				obs_property_set_long_description(p, D_TRANSLATE(D_DESC(ST_ROI_OFFSET_Y)));
				obs_property_float_set_suffix(p, " %");
			}
		}
	}
#ifdef ENABLE_PROFILING
	{
		obs_properties_add_button2(
			pr, "Profile", "Profile",
			[](obs_properties_t* props, obs_property_t* property, void* data) {
				return reinterpret_cast<face_tracking_instance*>(data)->button_profile(props, property);
			},
			data);
	}
#endif

	return pr;
}

std::shared_ptr<::nvidia::cuda::cuda> face_tracking_factory::get_cuda()
{
	return _cuda;
}

std::shared_ptr<::nvidia::cuda::context> face_tracking_factory::get_cuda_context()
{
	return _cuda_ctx;
}

std::shared_ptr<::nvidia::ar::ar> face_tracking_factory::get_ar()
{
	return _ar;
}

std::shared_ptr<face_tracking_factory> _filter_nvidia_face_tracking_factory_instance = nullptr;

void streamfx::filter::nvidia::face_tracking_factory::initialize()
{
	try {
		_filter_nvidia_face_tracking_factory_instance = std::make_shared<filter::nvidia::face_tracking_factory>();
	} catch (const std::exception& ex) {
		LOG_ERROR("<Nvidia Face Tracking Filter> %s", ex.what());
	}
}

void streamfx::filter::nvidia::face_tracking_factory::finalize()
{
	_filter_nvidia_face_tracking_factory_instance.reset();
}

std::shared_ptr<face_tracking_factory> streamfx::filter::nvidia::face_tracking_factory::get()
{
	return _filter_nvidia_face_tracking_factory_instance;
}
