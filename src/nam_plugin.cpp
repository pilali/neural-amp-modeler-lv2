#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <cassert>

#include "nam_plugin.h"

#define SMOOTH_EPSILON .0001f

#ifndef BYPASS_DB_THRESHOLD
#define BYPASS_DB_THRESHOLD -100
#endif

namespace NAM {
	Plugin::Plugin()
	{
		// prevent allocations on the audio thread
		currentModelPath.reserve(MAX_FILE_NAME + 1);

		bypassThresholdLinear = powf(10, BYPASS_DB_THRESHOLD * 0.05f);

//		NeuralAudio::NeuralModel::SetLSTMLoadMode(
//#ifdef LSTM_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
//		);
//
//		NeuralAudio::NeuralModel::SetWaveNetLoadMode(
//#ifdef WAVENET_PREFER_NAM
//			NeuralAudio::PreferNAMCore
//#else
//			NeuralAudio::PreferRTNeural
//#endif
		//);
	}

	Plugin::~Plugin()
	{
		delete currentModel;
	}

	bool Plugin::initialize(double sampleRate, const LV2_Feature* const* features) noexcept
	{
		this->sampleRate = sampleRate;

		// for fetching initial options, can be null
		LV2_Options_Option* options = nullptr;

		for (size_t i = 0; features[i]; ++i)
		{
			if (std::string(features[i]->URI) == std::string(LV2_URID__map))
				map = static_cast<LV2_URID_Map*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_WORKER__schedule))
				schedule = static_cast<LV2_Worker_Schedule*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_LOG__log))
				logger.log = static_cast<LV2_Log_Log*>(features[i]->data);
			else if (std::string(features[i]->URI) == std::string(LV2_OPTIONS__options))
				options = static_cast<LV2_Options_Option*>(features[i]->data);
		}
	
		lv2_log_logger_set_map(&logger, map);

		if (!map)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_URID__map);

			return false;
		}

		if (!schedule)
		{
			lv2_log_error(&logger, "Missing required feature: `%s`", LV2_WORKER__schedule);

			return false;
		}

		lv2_atom_forge_init(&atom_forge, map);

		uris.atom_Object = map->map(map->handle, LV2_ATOM__Object);
		uris.atom_Float = map->map(map->handle, LV2_ATOM__Float);
		uris.atom_Int = map->map(map->handle, LV2_ATOM__Int);
		uris.atom_Path = map->map(map->handle, LV2_ATOM__Path);
		uris.atom_URID = map->map(map->handle, LV2_ATOM__URID);
		uris.bufSize_maxBlockLength = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
		uris.patch_Set = map->map(map->handle, LV2_PATCH__Set);
		uris.patch_Get = map->map(map->handle, LV2_PATCH__Get);
		uris.patch_property = map->map(map->handle, LV2_PATCH__property);
		uris.patch_value = map->map(map->handle, LV2_PATCH__value);
		uris.units_frame = map->map(map->handle, LV2_UNITS__frame);

		uris.model_Path = map->map(map->handle, MODEL_URI);

		if (options != nullptr)
			options_set(this, options);

		// Ensure the buffered DSP rings exist even if the host never advertised
		// bufSize:maxBlockLength (some minimal hosts don't). Sized for the
		// current maxBufferSize, which is the default (512) unless options_set
		// already raised it via set_max_buffer_size above.
		allocate_buffered_rings(maxBufferSize);

		return true;
	}

	// runs on non-RT, can block or use [de]allocations
	LV2_Worker_Status Plugin::work(LV2_Handle instance, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle handle,
		uint32_t size, const void* data)
	{
		switch (*(const LV2WorkType*)data)
		{
			case kWorkTypeLoad:
			{
				auto msg = static_cast<const LV2LoadModelMsg*>(data);
				auto nam = static_cast<NAM::Plugin*>(instance);

				NeuralAudio::NeuralModel* model = nullptr;
				LV2SwitchModelMsg response = { kWorkTypeSwitch, {}, {} };
				LV2_Worker_Status result = LV2_WORKER_SUCCESS;

				try
				{
					// load model from path
					const size_t pathlen = strlen(msg->path);

					if (pathlen == 0 || pathlen >= MAX_FILE_NAME)
					{
						// avoid logging an error on an empty path.
						// but do clear the model.
						model = nullptr;
					}
					else
					{
						lv2_log_trace(&nam->logger, "Staging model change: `%s`\n", msg->path);

						model = NeuralAudio::NeuralModel::CreateFromFile(msg->path);
					}

					if (model != nullptr)
					{
						response.model = model;

						memcpy(response.path, msg->path, pathlen);
					}
				}
				catch (const std::exception&)
				{
				}

				if (model == nullptr)
				{
					response.path[0] = '\0';

					lv2_log_error(&nam->logger, "Unable to load model from: '%s'\n", msg->path);
				}

				respond(handle, sizeof(response), &response);

				return result;
			}

			case kWorkTypeFree:
			{
				auto msg = static_cast<const LV2FreeModelMsg*>(data);
				delete msg->model;

				return LV2_WORKER_SUCCESS;
			}

			case kWorkTypeSwitch:
				// should not happen!
				break;
		}

		return LV2_WORKER_ERR_UNKNOWN;
	}

	// runs on RT, right after process(), must not block or [de]allocate memory
	LV2_Worker_Status Plugin::work_response(LV2_Handle instance, uint32_t size,	const void* data)
	{
		if (*(const LV2WorkType*)data != kWorkTypeSwitch)
			return LV2_WORKER_ERR_UNKNOWN;

		auto msg = static_cast<const LV2SwitchModelMsg*>(data);
		auto nam = static_cast<NAM::Plugin*>(instance);

		// prepare reply for deleting old model
		LV2FreeModelMsg reply = { kWorkTypeFree, nam->currentModel };

		// swap current model with new one
		nam->currentModel = msg->model;
		nam->currentModelPath = msg->path;
		assert(nam->currentModelPath.capacity() >= MAX_FILE_NAME + 1);

		// Any in-flight samples in the buffered DSP rings belong to the previous
		// model, so flush them. Brief click on model change is expected; better
		// than a chunk of pre-processed audio surfacing through the new model.
		nam->reset_buffered_rings();

		if (nam->currentModel != nullptr)
		{
			int receptiveFieldSize = nam->currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSize > -1)
			{
				// A newly loaded model is prewarmed to have a silent sample history
				nam->silentSamples = receptiveFieldSize;
				nam->smartBypassed = true;
			}
		}

		// send reply
		nam->schedule->schedule_work(nam->schedule->handle, sizeof(reply), &reply);

		// report change to host/ui
		nam->write_current_path();

		return LV2_WORKER_SUCCESS;
	}

	void Plugin::set_max_buffer_size(int size) noexcept
	{
		maxBufferSize = size;

		NeuralAudio::NeuralModel::SetDefaultMaxAudioBufferSize(size);

		allocate_buffered_rings(size);
	}

	void Plugin::allocate_buffered_rings(int maxAudioBufferSize)
	{
		// Capacity needs to fit one full host buffer plus a partial block left
		// over from the previous call (worst case kBufferedBlockSize - 1).
		const size_t capacity = static_cast<size_t>(maxAudioBufferSize) + kBufferedBlockSize;
		if (bufferedIn.size() < capacity)
		{
			bufferedIn.assign(capacity, 0.0f);
		}
		if (bufferedOut.size() < capacity)
		{
			bufferedOut.assign(capacity, 0.0f);
		}
		reset_buffered_rings();
	}

	void Plugin::reset_buffered_rings() noexcept
	{
		bufferedInFill = 0;
		bufferedOutFill = 0;
	}

	void Plugin::process(uint32_t n_samples) noexcept
	{
		lv2_atom_forge_set_buffer(&atom_forge, (uint8_t*)ports.notify, ports.notify->atom.size);
		lv2_atom_forge_sequence_head(&atom_forge, &sequence_frame, uris.units_frame);

		LV2_ATOM_SEQUENCE_FOREACH(ports.control, event)
		{
			if (event->body.type == uris.atom_Object)
			{
				const auto obj = reinterpret_cast<LV2_Atom_Object*>(&event->body);
				if (obj->body.otype == uris.patch_Get)
				{
					write_current_path();
				}
				else if (obj->body.otype == uris.patch_Set)
				{
					const LV2_Atom* property = NULL;
					const LV2_Atom* file_path = NULL;

					lv2_atom_object_get(obj,
					                    uris.patch_property, &property,
					                    uris.patch_value, &file_path,
					                    0);

					if (property && property->type == uris.atom_URID &&
						((const LV2_Atom_URID*)property)->body == uris.model_Path &&
						file_path && file_path->type == uris.atom_Path &&
						file_path->size > 0 && file_path->size < MAX_FILE_NAME)
					{
						LV2LoadModelMsg msg = { kWorkTypeLoad, {} };
						memcpy(msg.path, file_path + 1, file_path->size);
						schedule->schedule_work(schedule->handle, sizeof(msg), &msg);
					}
				}
			}
		}

		if (*(ports.quality_scale) != qualityScale)
		{
			qualityScale = *(ports.quality_scale);

			NeuralAudio::NeuralModel::SetDefaultQualityScaleFactor(qualityScale);

			if (currentModel != nullptr)
			{
				currentModel->SetQualityScaleFactor(qualityScale);
			}
		}

		const bool bufferedModeNow = ports.buffered ? (*(ports.buffered) > 0.5f) : true;
		if (bufferedModeNow != bufferedModePrev)
		{
			// Mode flipped at runtime: any samples queued in the rings would
			// surface in the wrong path. Flush both sides; a brief click is
			// acceptable for a user-driven toggle.
			reset_buffered_rings();
			bufferedModePrev = bufferedModeNow;
		}

		if (bufferedModeNow && currentModel != nullptr)
		{
			process_buffered(n_samples);
		}
		else
		{
			process_unbuffered(n_samples);
		}
	}

	void Plugin::process_unbuffered(uint32_t n_samples) noexcept
	{
		float level;

		float modelInputAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			modelInputAdjustmentDB = currentModel->GetRecommendedInputDBAdjustment();

#ifdef SMART_BYPASS_ENABLED
			int receptiveFieldSamples = currentModel->GetReceptiveFieldSize();

			if (receptiveFieldSamples > -1)
			{
				for (unsigned int i = 0; i < n_samples; i++)
				{
					if (std::fabs(ports.audio_in[i]) <= bypassThresholdLinear)
					{
						silentSamples++;
					}
					else
					{
						silentSamples = 0;
					}
				}

				if (silentSamples >= (uint32_t)receptiveFieldSamples)
				{
					silentSamples = (uint32_t)receptiveFieldSamples;	// Prevent silentSamples growing and eventually overflowing uint32

					if (smartBypassed)
					{
						for (unsigned int i = 0; i < n_samples; i++)
						{
							ports.audio_out[i] = ports.audio_in[i];
						}

						return;
					}

					smartBypassed = true; // If we aren't already, we'll be bypassed on the next process call
				}
				else
					smartBypassed = false;
			}
#endif
		}

		// convert input level from db
		float desiredInputLevel = powf(10, (*(ports.input_level) + modelInputAdjustmentDB) * 0.05f);

		if (fabs(desiredInputLevel - inputLevel) > SMOOTH_EPSILON)
		{
			level = inputLevel;
			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredInputLevel);

				ports.audio_out[i] = ports.audio_in[i] * level;
			}

			inputLevel = level;
		}
		else
		{
			level = inputLevel = desiredInputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_in[i] * level;
			}
		}

		float modelLoudnessAdjustmentDB = 0;

		if (currentModel != nullptr)
		{
			currentModel->Process(ports.audio_out, ports.audio_out, n_samples);

			modelLoudnessAdjustmentDB = currentModel->GetRecommendedOutputDBAdjustment();
		}

		// Convert output level from db
		float desiredOutputLevel = powf(10, (*(ports.output_level) + modelLoudnessAdjustmentDB) * 0.05f);

		if (fabs(desiredOutputLevel - outputLevel) > SMOOTH_EPSILON)
		{
			level = outputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				// do very basic smoothing
				level = (.99f * level) + (.01f * desiredOutputLevel);

				ports.audio_out[i] = ports.audio_out[i] * level;
			}

			outputLevel = level;
		}
		else
		{
			level = outputLevel = desiredOutputLevel;

			for (unsigned int i = 0; i < n_samples; i++)
			{
				ports.audio_out[i] = ports.audio_out[i] * level;
			}
		}
	}

	void Plugin::process_buffered(uint32_t n_samples) noexcept
	{
		// Defensive: if the host ever calls with a chunk bigger than what we
		// pre-allocated for, fall back to the unbuffered path. Shouldn't happen
		// in practice — bufSize maxBlockLength is honoured by mod-host.
		if (n_samples + bufferedInFill > bufferedIn.size())
		{
			process_unbuffered(n_samples);
			return;
		}

		const float modelInputAdjustmentDB = currentModel->GetRecommendedInputDBAdjustment();
		const float modelLoudnessAdjustmentDB = currentModel->GetRecommendedOutputDBAdjustment();
		const float desiredInputLevel = powf(10, (*(ports.input_level) + modelInputAdjustmentDB) * 0.05f);
		const float desiredOutputLevel = powf(10, (*(ports.output_level) + modelLoudnessAdjustmentDB) * 0.05f);

		// 1. Apply input level (with smoothing) and push into the input ring.
		const bool inputSmoothing = fabs(desiredInputLevel - inputLevel) > SMOOTH_EPSILON;
		float level = inputLevel;
		float* inWrite = bufferedIn.data() + bufferedInFill;
		if (inputSmoothing)
		{
			for (uint32_t i = 0; i < n_samples; i++)
			{
				level = (.99f * level) + (.01f * desiredInputLevel);
				inWrite[i] = ports.audio_in[i] * level;
			}
			inputLevel = level;
		}
		else
		{
			level = inputLevel = desiredInputLevel;
			for (uint32_t i = 0; i < n_samples; i++)
			{
				inWrite[i] = ports.audio_in[i] * level;
			}
		}
		bufferedInFill += n_samples;

		// 2. Process as many full blocks as we have queued. The model writes
		//    directly into the output ring (Process supports distinct in/out
		//    pointers), so we avoid an extra memcpy per block. Consumed input
		//    samples accumulate in inOffset; we collapse them with a single
		//    memmove after the loop instead of shifting once per block.
		const size_t outCapacity = bufferedOut.size();
		size_t inOffset = 0;
		while (bufferedInFill - inOffset >= static_cast<size_t>(kBufferedBlockSize) &&
		       bufferedOutFill + kBufferedBlockSize <= outCapacity)
		{
			currentModel->Process(bufferedIn.data() + inOffset,
			                      bufferedOut.data() + bufferedOutFill,
			                      kBufferedBlockSize);
			inOffset += kBufferedBlockSize;
			bufferedOutFill += kBufferedBlockSize;
		}
		if (inOffset > 0)
		{
			const size_t remaining = bufferedInFill - inOffset;
			if (remaining > 0)
			{
				std::memmove(bufferedIn.data(),
				             bufferedIn.data() + inOffset,
				             remaining * sizeof(float));
			}
			bufferedInFill = remaining;
		}

		// 3. Drain up to n_samples from the output ring, applying output level
		//    smoothing. If the ring doesn't yet hold enough (the typical case
		//    on the first few callbacks after a flush), pad the tail with zeros
		//    — that's the wrapper's intrinsic startup latency.
		const uint32_t avail = static_cast<uint32_t>(std::min<size_t>(bufferedOutFill, n_samples));
		const bool outputSmoothing = fabs(desiredOutputLevel - outputLevel) > SMOOTH_EPSILON;
		level = outputLevel;
		if (outputSmoothing)
		{
			for (uint32_t i = 0; i < avail; i++)
			{
				level = (.99f * level) + (.01f * desiredOutputLevel);
				ports.audio_out[i] = bufferedOut[i] * level;
			}
			outputLevel = level;
		}
		else
		{
			level = outputLevel = desiredOutputLevel;
			for (uint32_t i = 0; i < avail; i++)
			{
				ports.audio_out[i] = bufferedOut[i] * level;
			}
		}
		for (uint32_t i = avail; i < n_samples; i++)
		{
			ports.audio_out[i] = 0.0f;
		}

		const size_t outRemaining = bufferedOutFill - avail;
		if (outRemaining > 0)
		{
			std::memmove(bufferedOut.data(),
			             bufferedOut.data() + avail,
			             outRemaining * sizeof(float));
		}
		bufferedOutFill = outRemaining;
	}

	uint32_t Plugin::options_get(LV2_Handle, LV2_Options_Option*)
	{
		// currently unused
		return LV2_OPTIONS_ERR_UNKNOWN;
	}

	uint32_t Plugin::options_set(LV2_Handle instance, const LV2_Options_Option* options)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		for (int i=0; options[i].key && options[i].type; ++i)
		{
			if (options[i].key == nam->uris.bufSize_maxBlockLength && options[i].type == nam->uris.atom_Int)
			{
				nam->set_max_buffer_size(*(const int32_t*)options[i].value);
				break;
			}
		}

		return LV2_OPTIONS_SUCCESS;
	}

	LV2_State_Status Plugin::save(LV2_Handle instance, LV2_State_Store_Function store, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		lv2_log_trace(&nam->logger, "Saving state\n");

		if (!nam->currentModel)
		{
			return LV2_STATE_SUCCESS;
		}

		LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

		if (map_path == nullptr)
		{
			lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

			return LV2_STATE_ERR_NO_FEATURE;
		}

		// Map absolute sample path to an abstract state path
		char* apath = map_path->abstract_path(map_path->handle, nam->currentModelPath.c_str());

		store(handle, nam->uris.model_Path, apath, strlen(apath) + 1, nam->uris.atom_Path,
			LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

		LV2_State_Free_Path* free_path = (LV2_State_Free_Path *)lv2_features_data(features, LV2_STATE__freePath);

		if (free_path != nullptr)
		{
			free_path->free_path(free_path->handle, apath);
		}
		else
		{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
			free(apath);
#endif
		}

		return LV2_STATE_SUCCESS;
	}

	LV2_State_Status Plugin::restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle, 
		uint32_t flags, const LV2_Feature* const* features)
	{
		auto nam = static_cast<NAM::Plugin*>(instance);

		// Get model_Path from state
		size_t      size     = 0;
		uint32_t    type     = 0;
		uint32_t    valflags = 0;
		const void* value = retrieve(handle, nam->uris.model_Path, &size, &type, &valflags);

		lv2_log_trace(&nam->logger, "Restoring model '%s'\n", (const char*)value);

		NAM::LV2LoadModelMsg msg = { NAM::kWorkTypeLoad, {} };

		LV2_State_Status result = LV2_STATE_SUCCESS;

		// Check if a path is set
		if (!value || (type != nam->uris.atom_Path))
		{
			msg.path[0] = '\0';
		}
		else
		{
			LV2_State_Map_Path* map_path = (LV2_State_Map_Path*)lv2_features_data(features, LV2_STATE__mapPath);

			if (map_path == nullptr)
			{
				lv2_log_error(&nam->logger, "LV2_STATE__mapPath unsupported by host\n");

				return LV2_STATE_ERR_NO_FEATURE;
			}

			// Map abstract state path to absolute path
			char* path = map_path->absolute_path(map_path->handle, (const char *)value);

			size_t pathLen = strlen(path);

			if (pathLen >= MAX_FILE_NAME)
			{
				lv2_log_error(&nam->logger, "Model path is too long (max %u chars)\n", MAX_FILE_NAME);

				result = LV2_STATE_ERR_UNKNOWN;
			}
			else
			{
				memcpy(msg.path, path, pathLen);
			}

			LV2_State_Free_Path* free_path = (LV2_State_Free_Path*)lv2_features_data(features, LV2_STATE__freePath);

			if (free_path != nullptr)
			{
				free_path->free_path(free_path->handle, path);
			}
			else
			{
#ifndef _WIN32	// Can't free host-allocated memory on plugin side under Windows
				free(path);
#endif
			}
		}

		if (result == LV2_STATE_SUCCESS)
		{
			// Schedule model to be loaded by the provided worker
			nam->schedule->schedule_work(nam->schedule->handle, sizeof(msg), &msg);

			nam->currentModelPath = msg.path;
		}

		return result;
	}

	void Plugin::write_current_path()
	{
		LV2_Atom_Forge_Frame frame;

		lv2_atom_forge_frame_time(&atom_forge, 0);
		lv2_atom_forge_object(&atom_forge, &frame, 0, uris.patch_Set);

		lv2_atom_forge_key(&atom_forge, uris.patch_property);
		lv2_atom_forge_urid(&atom_forge, uris.model_Path);
		lv2_atom_forge_key(&atom_forge, uris.patch_value);
		lv2_atom_forge_path(&atom_forge, currentModelPath.c_str(), (uint32_t)currentModelPath.length() + 1);

		lv2_atom_forge_pop(&atom_forge, &frame);
	}
}
