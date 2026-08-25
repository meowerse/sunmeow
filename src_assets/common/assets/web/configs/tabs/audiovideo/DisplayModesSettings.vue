<script setup>
import { ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config',
])
const config = ref(props.config)
</script>

<template>
  <!--max_bitrate-->
  <div class="mb-3">
    <label for="max_bitrate" class="form-label">{{ $t("config.max_bitrate") }}</label>
    <input type="number" class="form-control" id="max_bitrate" placeholder="0" v-model="config.max_bitrate" />
    <div class="form-text">{{ $t("config.max_bitrate_desc") }}</div>
  </div>

  <!--adaptive_bitrate_min-->
  <div class="mb-3">
    <label for="adaptive_bitrate_min" class="form-label">{{ $t("config.adaptive_bitrate_min") }}</label>
    <input type="number" min="0" class="form-control" id="adaptive_bitrate_min" placeholder="0" v-model="config.adaptive_bitrate_min" />
    <div class="form-text">{{ $t("config.adaptive_bitrate_min_desc") }}</div>
  </div>

  <!--adaptive_bitrate_max-->
  <div class="mb-3">
    <label for="adaptive_bitrate_max" class="form-label">{{ $t("config.adaptive_bitrate_max") }}</label>
    <input type="number" min="0" class="form-control" id="adaptive_bitrate_max" placeholder="0" v-model="config.adaptive_bitrate_max" />
    <div class="form-text">{{ $t("config.adaptive_bitrate_max_desc") }}</div>
  </div>

  <!--minimum_fps_target-->
  <div class="mb-3">
    <label for="minimum_fps_target" class="form-label">{{ $t("config.minimum_fps_target") }}</label>
    <input type="number" min="0" max="1000" class="form-control" id="minimum_fps_target" placeholder="0" v-model="config.minimum_fps_target" />
    <div class="form-text">{{ $t("config.minimum_fps_target_desc") }}</div>
  </div>
</template>

<style scoped>
.ms-item {
  background-color: var(--bs-dark-bg-subtle);
  font-size: 12px;
  font-weight: bold;
}
</style>
