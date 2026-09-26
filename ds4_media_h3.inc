/* Grafo MiniMax H3 (image->video) con segnaposto, da h3-singleshot-firstframe-noupscale.json
 * (video-it, install keys-heretic). Generato: non modificare a mano. Segnaposto sostituiti da
 * ds4_media_video: __PROMPT__ __IMAGE__ __PREFIX__ (stringhe JSON-escaped), __WIDTH__ __HEIGHT__
 * __LENGTH__ __SEED__ __STEPS__ (numeri). */
static const char DS4_MEDIA_H3_TEMPLATE[] =
    "{\"6\": {\"class_type\": \"UNETLoader\", \"inputs\": {\"unet_name\": \"minimax_h3_fl2va_pruned_int8_convrot.safetensors\","
    " \"weight_dtype\": \"default\"}}, \"50\": {\"class_type\": \"PathchSageAttentionKJ\", \"inputs\": {\"model\": [\"6\", 0], \"sag"
    "e_attention\": \"auto\", \"allow_compile\": false}}, \"7\": {\"class_type\": \"SolAttnPatch\", \"inputs\": {\"model\": [\"50\","
    " 0], \"tau\": 1.3, \"start_percent\": 0.2, \"end_percent\": 0.9, \"min_tokens\": 4096, \"int8_qk\": true, \"sink_conditio"
    "ning\": \"exact_kv_and_rows\", \"morton\": false, \"morton_curve\": \"2d_frame\", \"verbose\": true, \"use_tma\": true, \"in"
    "t8_pv\": true, \"dense_blocks\": \"\"}}, \"51\": {\"class_type\": \"SpectrumApplyMiniMaxH3\", \"inputs\": {\"model\": [\"7\", 0"
    "], \"enabled\": true, \"blend_weight\": 0.5, \"degree\": 4, \"ridge_lambda\": 0.1, \"window_size\": 2.0, \"flex_window\": "
    "0.75, \"warmup_steps\": 5, \"tail_actual_steps\": 1, \"max_history\": 8, \"debug\": false, \"history_storage\": \"system_"
    "ram\", \"bootstrap_first_forecast\": false}}, \"8\": {\"class_type\": \"H3FirstBlockCache\", \"inputs\": {\"model\": [\"51\","
    " 0], \"threshold\": 0.08, \"start_step\": 3, \"end_dense_steps\": 2, \"max_consecutive_skips\": 2}}, \"13\": {\"class_typ"
    "e\": \"CLIPLoader\", \"inputs\": {\"clip_name\": \"H3/qwen3vl_32b_h3_ultra_uncensored_heretic_int8_convrot.safetensors"
    "\", \"type\": \"minimax\", \"device\": \"default\"}}, \"11\": {\"class_type\": \"VAELoader\", \"inputs\": {\"vae_name\": \"minimax"
    "_h3_video_vae_fp16.safetensors\"}}, \"24\": {\"class_type\": \"VAELoader\", \"inputs\": {\"vae_name\": \"minimax_h3_audio_"
    "vae_fp32.safetensors\"}}, \"104\": {\"class_type\": \"MiniMaxH3ImageToVideo\", \"inputs\": {\"clip\": [\"13\", 0], \"vae\": ["
    "\"11\", 0], \"prompt\": \"__PROMPT__\", \"width\": __WIDTH__, \"height\": __HEIGHT__, \"length\": __LENGTH__, \"first_frame"
    "\": [\"105\", 0]}}, \"15\": {\"class_type\": \"RandomNoise\", \"inputs\": {\"noise_seed\": __SEED__}}, \"17\": {\"class_type\":"
    " \"KSamplerSelect\", \"inputs\": {\"sampler_name\": \"res_multistep\"}}, \"9\": {\"class_type\": \"BasicScheduler\", \"inputs"
    "\": {\"model\": [\"8\", 0], \"scheduler\": \"simple\", \"steps\": __STEPS__, \"denoise\": 1.0}}, \"16\": {\"class_type\": \"Basi"
    "cGuider\", \"inputs\": {\"model\": [\"8\", 0], \"conditioning\": [\"104\", 0]}}, \"14\": {\"class_type\": \"SamplerCustomAdvan"
    "ced\", \"inputs\": {\"noise\": [\"15\", 0], \"guider\": [\"16\", 0], \"sampler\": [\"17\", 0], \"sigmas\": [\"9\", 0], \"latent_im"
    "age\": [\"104\", 1]}}, \"10\": {\"class_type\": \"VAEDecode\", \"inputs\": {\"samples\": [\"14\", 0], \"vae\": [\"11\", 0]}}, \"23"
    "\": {\"class_type\": \"VAEDecodeAudio\", \"inputs\": {\"samples\": [\"14\", 0], \"vae\": [\"24\", 0]}}, \"91\": {\"class_type\": "
    "\"CreateVideo\", \"inputs\": {\"images\": [\"10\", 0], \"fps\": 24, \"audio\": [\"23\", 0]}}, \"92\": {\"class_type\": \"SaveVide"
    "o\", \"inputs\": {\"video\": [\"91\", 0], \"filename_prefix\": \"__PREFIX__\", \"format\": \"auto\", \"codec\": \"auto\"}}, \"105\""
    ": {\"class_type\": \"LoadImage\", \"inputs\": {\"image\": \"__IMAGE__\"}}}";
