# vk-flip-meter
Software flip metering tool to use with Nvidia RTX 4000 series GPUS

Usage:
  # Standard FG (1x):
  ENABLE_LAYER_cpu_flip_meter=1 %command%

  # MFG 4x:
  ENABLE_LAYER_cpu_flip_meter=1 FLM_MFG_MULTIPLIER=3 %command%

  # MFG 4x with target FPS cap + verbose:
  ENABLE_LAYER_cpu_flip_meter=1 FLM_MFG_MULTIPLIER=3 FLM_VERBOSE=1 FLM_TARGET_FPS=60 %command%

  (Base FPS target) or 0 for automatic adaptation. Also run with FLM_VERBOSE=1 for the first time to make sure its working correctly.
