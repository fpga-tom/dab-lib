# API Overview

This file summarizes the main exported functions in `dab-lib`, based on the headers under `include/dab/`.

## Core Receiver Flow

### `dab_receiver_init`
Initializes a `dab_receiver_t` with its IQ block pool and filled-block queue. Uses defaults when `queue_capacity` or `block_count` are zero.

### `dab_receiver_shutdown`
Stops queues, destroys internal storage, frees allocated receiver buffers, and resets the receiver struct.

### `dab_frontend_default_config`
Fills `dab_frontend_config_t` with default frontend settings. Currently this sets the DC-removal smoothing factor.

### `dab_frontend_init`
Initializes frontend state and validates configuration values.

### `dab_frontend_reset`
Clears the frontend DC estimate and processed-sample counters.

### `dab_frontend_process`
Converts interleaved `int16` IQ samples into planar `float` I/Q buffers and applies streaming DC removal.

## Null Detection And Synchronization

### `dab_null_detect_default_config`
Fills `dab_null_detect_config_t` with default null-symbol detector thresholds and window settings.

### `dab_null_detect_init`
Initializes the null detector and validates the configured averaging windows and thresholds.

### `dab_null_detect_reset`
Clears all null-detector history, running averages, and in-progress detection state.

### `dab_null_detect_process`
Consumes one frontend block and reports a completed `dab_null_event_t` when a likely null symbol is detected.

### `dab_null_detect_process_i16`
Runs the same null-detection logic directly on raw SDR `int16` IQ blocks.

### `dab_sync_default_config`
Loads default Mode I synchronization parameters such as expected frame size, null length, and PRS score thresholds.

### `dab_sync_init`
Initializes the sync state machine and validates acquisition/tracking thresholds.

### `dab_sync_reset`
Returns the sync state machine to idle.

### `dab_sync_on_null_candidate`
Moves sync into acquisition mode using a detected null-symbol event as the tentative frame start.

### `dab_sync_on_prs_acquire`
Processes an acquisition-phase PRS result. Locks the synchronizer if the PRS score is strong enough.

### `dab_sync_on_prs_track`
Processes a tracking-phase PRS result. Updates timing and frequency estimates, or falls back to acquisition after repeated bad frames.

### `dab_sync_predict_next_frame_start`
Predicts the next frame start sample from the current locked timing state.

## PRS Acquisition, FFT, And Scoring

### `dab_prs_acquire_default_config`
Loads default timing parameters for building a PRS search candidate after a null symbol.

### `dab_prs_acquire_init`
Initializes PRS acquisition configuration and validates the symbol geometry.

### `dab_prs_acquire_reset`
Resets acquisition state. This is effectively a no-op today because the object is configuration-only.

### `dab_prs_candidate_from_null`
Builds a `dab_prs_candidate_t` from a detected null symbol, including the expected PRS symbol and search window.

### `dab_prs_candidate_search_start_sample`
Returns the first sample index that should be searched for the PRS FFT peak.

### `dab_prs_candidate_search_end_sample`
Returns the end of the PRS search window.

### `dab_prs_fft_default_config`
Loads default FFT settings for PRS analysis.

### `dab_prs_fft_init`
Initializes FFT tables and selects the backend implementation.

### `dab_prs_fft_reset`
Resets FFT state. The current implementation is stateless, so this mainly preserves API symmetry.

### `dab_prs_fft_execute_i16`
Runs the PRS FFT on raw SDR `int16` IQ samples starting at a specified FFT window.

### `dab_prs_fft_execute_i16_cfo`
Runs the same FFT while pre-correcting for a supplied carrier-frequency offset.

### `dab_prs_fft_apply_bin_shift`
Rotates FFT bins in-place to compensate for an integer carrier offset.

### `dab_prs_template_init`
Initializes a PRS reference template over caller-provided arrays.

### `dab_prs_template_clear`
Clears all template bins and active-bin markers.

### `dab_prs_template_set_bin`
Sets one complex reference bin in a PRS template and marks it active.

### `dab_prs_template_active_count`
Returns the number of active bins currently present in the template.

### `dab_prs_template_load_bins`
Loads a whole set of reference bins into a PRS template.

### `dab_prs_template_load_mode_i`
Loads the built-in DAB Mode I PRS reference template.

### `dab_prs_score_default_config`
Loads default PRS scoring parameters such as sample rate, guard length, and FFT size.

### `dab_prs_score_init`
Initializes PRS scoring configuration.

### `dab_prs_score_reset`
Resets PRS scoring state. The current scorer is effectively stateless.

### `dab_prs_score_cp_metric_i16`
Computes a cyclic-prefix based coarse score and coarse frequency estimate for one FFT position.

### `dab_prs_score_search_i16`
Searches a candidate timing window for the best PRS peak and returns a `dab_prs_result_t`.

### `dab_prs_score_reference_f32`
Correlates complex float samples against a reference sequence and reports score, energy, and phase information.

### `dab_prs_score_reference_template_f32`
Scores FFT bins against a `dab_prs_template_t`.

### `dab_prs_score_reference_bins_diff_f32`
Scores differential bins while scanning a configurable carrier-offset range.

### `dab_prs_score_reference_mode_i_diff_f32`
Convenience helper for Mode I differential PRS scoring.

### `dab_prs_score_symbol_i16`
Runs FFT plus template scoring for one symbol from raw SDR input.

### `dab_prs_score_symbol_i16_mode_i_diff`
Mode I convenience wrapper for symbol scoring using the built-in differential PRS reference.

### `dab_prs_score_symbol_i16_mode_i_diff_cfo`
Same as the previous helper, but with an explicit carrier-frequency correction.

## OFDM Symbol Processing

### `dab_ofdm_default_config`
Loads the fixed DAB Mode I OFDM geometry.

### `dab_ofdm_init`
Initializes OFDM state, PRS reference carriers, and the frequency deinterleaver map.

### `dab_ofdm_reset`
Clears previous-symbol and channel-estimate state.

### `dab_ofdm_extract_active_carriers_mode_i`
Extracts the 1537 active DAB Mode I carriers from FFT-bin order into active-carrier order.

### `dab_ofdm_diff_demodulate_mode_i`
Differentially demodulates the current symbol against the previous active-carrier symbol.

### `dab_ofdm_frequency_deinterleave_mode_i`
Applies the DAB Mode I frequency deinterleaver to differential carriers.

### `dab_ofdm_qpsk_soft_decode_mode_i`
Converts deinterleaved QPSK carriers into soft bits for the `a` and `b` streams.

### `dab_ofdm_process_mode_i`
High-level Mode I symbol step: extract carriers, estimate or apply channel correction, optionally differential-demodulate, deinterleave, and soft-decode.

## Frame Assembly And Channel Decoding

### `dab_tframe_init`
Initializes a transmission-frame assembler over caller-supplied FIC, MSC, and full-frame softbit buffers.

### `dab_tframe_reset`
Clears frame assembly progress and current frame-start tracking.

### `dab_tframe_push_symbol`
Appends one soft-decoded OFDM symbol into the current transmission frame and reports when all 75 symbols are complete.

### `dab_fic_init`
Initializes FIC decoder state and counters.

### `dab_fic_reset`
Clears FIC decoder statistics and decoded-output buffers.

### `dab_fic_process_frame`
Decodes the FIC portion of one completed frame, updates CRC status for all FIBs, stores successfully decoded FIB bytes, and parses valid FIB payloads into the in-memory FIC repo.

### `dab_fic_debug_analyze_frame`
Runs multiple debug decoding variants over a frame and reports which combinations produced valid FIB CRCs.

### `dab_fic_debug_decode_block`
Performs an expanded debug decode of one FIC block, including intermediate depuncturing and packing artifacts.

### `dab_fic_cursor_init`
Initializes a cursor over a raw FIB payload for sequential FIG parsing.

### `dab_fic_cursor_next_fig`
Returns the next FIG envelope from a FIB payload and stops cleanly on FIG type-7 padding.

### `dab_fic_fig_view_from_raw`
Normalizes a raw FIG into a parsed view, including the FIG0 inner header fields.

### `dab_fic_bit_reader_init`
Initializes a bit reader used for packed FIG payloads such as FIG0/1 subchannel organization.

### `dab_fic_bit_reader_get`
Reads an arbitrary-width big-endian bit field from a packed FIG payload.

### `dab_fic_parse_fib_payload`
Parses one valid 30-byte FIB payload into a `dab_fic_repo_t`, covering the current minimal FIG support set: FIG0/0, FIG0/1, FIG0/2, and FIG1 labels.

### `dab_msc_init`
Initializes MSC state.

### `dab_msc_reset`
Clears MSC frame counters, decoder runtimes, and stored sample values while preserving the configured service filter.

### `dab_msc_set_service_filter`
Restricts subsequent MSC convolutional deinterleaving and FEC decoding to the stream-audio subchannel mapped to one service ID. FIC-derived ensemble metadata and component bindings remain available for all services.

### `dab_msc_clear_service_filter`
Restores the default behavior of decoding every valid MSC subchannel.

### `dab_msc_build_layout`
Normalizes parsed FIG0/1 and FIG0/2 data into MSC subchannel and service-component mappings, including derived CU size and bitrate for UEP/EEP profiles.

### `dab_msc_find_subchannel`
Looks up one normalized MSC subchannel by `SubChId` from a prepared layout.

### `dab_msc_find_decoded_component`
Looks up one decoded MSC service component by `SID` and component index after `dab_msc_process_frame_with_fic`.

### `dab_msc_find_primary_decoded_component`
Looks up the primary decoded MSC service component for one `SID`, falling back to component index 0 when no component is flagged primary.

### `dab_msc_get_component_payload`
Builds a pull-style view over the currently decoded CIF payloads for one resolved service component, including `tmid`-based component kind and direct pointers into the decoded subchannel storage.

### `dab_msc_get_primary_component_payload`
Convenience wrapper that resolves the primary component for one service and returns its decoded CIF payload view.

### `dab_msc_ingest_component_payload`
Feeds one decoded component payload into the MSC content layer. For `tmid == 0`, this accumulates DAB+ CIF payloads into a 120-CIF superframe assembler and updates the latest synchronized audio superframe view.

### `dab_msc_get_dabplus_superframe`
Returns the latest synchronized DAB+ superframe view for one decoded audio component, including parsed audio flags and AU boundaries.

### `dab_msc_get_primary_dabplus_superframe`
Convenience wrapper that resolves the primary component for one service and returns its latest synchronized DAB+ superframe.

### `dab_msc_get_subchannel_softbits`
Returns the MSC softbit slice for one subchannel within one CIF of the current frame.

### `dab_msc_process_frame`
Consumes the MSC softbits for one frame and records simple frame-level state. This is currently a placeholder rather than a full MSC decoder.

### `dab_msc_process_frame_with_fic`
Consumes one MSC frame, builds the current subchannel/service layout from parsed FIC data, decodes routed subchannels, records decoded component bindings, and feeds stream-audio components into the DAB+ superframe assembler.

## Threading And Buffer Utilities

### `dab_pc_queue_init`
Initializes a bounded producer-consumer queue over caller-provided storage.

### `dab_pc_queue_destroy`
Destroys queue synchronization primitives.

### `dab_pc_queue_push`
Blocking push into the queue.

### `dab_pc_queue_pop`
Blocking pop from the queue.

### `dab_pc_queue_try_push`
Non-blocking push that fails immediately if the queue is full or stopped.

### `dab_pc_queue_try_pop`
Non-blocking pop that fails immediately if the queue is empty or stopped.

### `dab_pc_queue_stop`
Marks the queue as stopped and wakes blocked producers or consumers.

### `dab_iq_block_pool_init`
Initializes a pool of reusable IQ blocks backed by a free-block queue.

### `dab_iq_block_pool_destroy`
Destroys the block-pool queue state.

### `dab_iq_block_acquire`
Gets a free IQ block from the pool.

### `dab_iq_block_release`
Returns an IQ block back to the pool.

## SDR Hardware Abstraction

### `dab_sdr_device_count`
Returns the number of SDR devices currently visible through the HAL.

### `dab_sdr_device_desc`
Returns the static description for one enumerated SDR device.

### `dab_sdr_device_open`
Opens a device selected by driver, device ID, or serial number.

### `dab_sdr_device_close`
Closes and destroys an opened SDR device handle.

### `dab_sdr_stream_close`
Closes and destroys an opened SDR stream handle.

### `dab_sdrplay_default_config`
Loads default SDRplay-specific tuning and gain settings.

### `dab_sdrplay_configure_device`
Applies SDRplay-specific configuration to a device already opened through the generic HAL.

### `dab_sdrplay_stream_dropped_blocks`
Returns the number of dropped blocks observed by the SDRplay stream bridge.

### `dab_rtlsdr_default_config`
Loads default RTL-SDR-specific gain, PPM, and bias-tee settings.

### `dab_rtlsdr_configure_device`
Applies RTL-SDR-specific configuration to a device already opened through the generic HAL.

### `dab_rtlsdr_stream_dropped_blocks`
Returns the number of dropped blocks observed by the RTL-SDR stream bridge.

## OS And SIMD Support

### `dab_time_monotonic_ns`
Returns a monotonic timestamp in nanoseconds.

### `dab_mutex_init`, `dab_mutex_destroy`, `dab_mutex_lock`, `dab_mutex_unlock`
Portable mutex wrappers used by the queue and thread helpers.

### `dab_cond_init`, `dab_cond_destroy`, `dab_cond_wait`, `dab_cond_signal`, `dab_cond_broadcast`
Portable condition-variable wrappers used by the producer-consumer queue.

### `dab_thread_create`, `dab_thread_join`
Portable thread creation and join helpers.

### `dab_shared_lib_open`, `dab_shared_lib_symbol`, `dab_shared_lib_close`
Portable shared-library loading helpers used by the SDRplay and RTL-SDR dynamic-loader paths.

### `dab_simd_init`
Detects the best SIMD backend for the current platform.

### `dab_simd_get_ops`
Returns the active SIMD operations table used by the frontend and other low-level code.

### `dab_simd_scalar_ops`, `dab_simd_avx2_ops`, `dab_simd_neon_ops`
Return specific backend operation tables for explicit selection or testing.
