/*
 * Copyright (C) 2018 Mentor, a Siemens Business
 * Copyright (C) 2018 Xilinx, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "hwc-platform-zynqmp"

#include "drmresources.h"
#include "platform.h"
#include "platformzynqmp.h"


#include <drm/drm_fourcc.h>
#include <cinttypes>
#include <stdatomic.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <log/log.h>
#include <hardware/gralloc.h>
#include "gralloc_priv.h"

#define MALI_ALIGN(value, base) (((value) + ((base)-1)) & ~((base)-1))
namespace android {

Importer *Importer::CreateInstance(DrmResources *drm) {
  ZynqmpImporter *importer = new ZynqmpImporter(drm);
  if (!importer)
    return NULL;

  int ret = importer->Init();
  if (ret) {
    ALOGE("Failed to initialize the zynqmp importer %d", ret);
    delete importer;
    return NULL;
  }
  return importer;
}

ZynqmpImporter::ZynqmpImporter(DrmResources *drm) : DrmGenericImporter(drm), drm_(drm) {
}

ZynqmpImporter::~ZynqmpImporter() {
}

int ZynqmpImporter::Init() {
  int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                          (const hw_module_t **)&gralloc_);
  if (ret) {
    ALOGE("Failed to open gralloc module %d", ret);
    return ret;
  }

  if (strcasecmp(gralloc_->common.author, "ARM Ltd."))
    ALOGW("Using non-ARM gralloc module: %s/%s\n", gralloc_->common.name,
          gralloc_->common.author);

  return 0;
}

int ZynqmpImporter::CheckBuffer(buffer_handle_t handle) {
  private_handle_t const *hnd =
      reinterpret_cast<private_handle_t const *>(handle);
  std::pair<int32_t, int32_t> max = drm_->max_resolution();

  if (!hnd) {
    ALOGI(" BAD BUFFER");
    return -EINVAL;
  }

  if (hnd->width > max.first || hnd->height > max.second){
    ALOGI(" BAD BUFFER");
    return -EINVAL;
  }
  return 0;
}

uint32_t ZynqmpImporter::ConvertHalFormatToDrm(uint32_t hal_format) {
  switch (hal_format) {
    case HAL_PIXEL_FORMAT_RGB_888:
      return DRM_FORMAT_BGR888;
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return DRM_FORMAT_ARGB8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
      return DRM_FORMAT_XBGR8888;
    case HAL_PIXEL_FORMAT_RGBA_8888:
      return DRM_FORMAT_ABGR8888;
    case HAL_PIXEL_FORMAT_RGB_565:
      return DRM_FORMAT_BGR565;
    case HAL_PIXEL_FORMAT_YV12:
      return DRM_FORMAT_YVU420;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
      return DRM_FORMAT_NV12;
    default:
      ALOGE("Cannot convert hal format to drm format %u", hal_format);
      return -EINVAL;
  }
}

int ZynqmpImporter::IsRgbBuffer(buffer_handle_t handle) {
  private_handle_t const *hnd = reinterpret_cast < private_handle_t const *>(handle);
  if (!hnd)
    return -EINVAL;

  switch (hnd->format){
    case HAL_PIXEL_FORMAT_RGB_888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGB_565:
      return 1;
    default:
      return 0;
  }
}

int ZynqmpImporter::ImportBuffer(buffer_handle_t handle, hwc_drm_bo_t *bo) {
  private_handle_t const *hnd = reinterpret_cast < private_handle_t const *>(handle);
  if (!hnd)
    return -EINVAL;

  uint32_t gem_handle;
  int ret = drmPrimeFDToHandle(drm_->fd(), hnd->share_fd, &gem_handle);
  if (ret) {
    ALOGE("failed to import prime fd %d ret=%d", hnd->share_fd, ret);
    ALOGE("~~~ fmt=0x%x width=%d height=%d usage=0x%x", hnd->format, hnd->width, hnd->height, hnd->usage);
    return ret;
  }

  int32_t fmt = ConvertHalFormatToDrm(hnd->format);
  if (fmt < 0)
    return fmt;

  memset(bo, 0, sizeof(hwc_drm_bo_t));
  bo->width = hnd->width;
  bo->height = hnd->height;
  bo->format = fmt;
  bo->usage = hnd->usage;
  bo->pitches[0] = hnd->byte_stride;
  bo->gem_handles[0] = gem_handle;
  bo->offsets[0] = 0;

  switch (fmt) {
    case DRM_FORMAT_YVU420: {
      int adjusted_height = MALI_ALIGN(hnd->height, 2);
      int y_size = adjusted_height * hnd->byte_stride;
      int vu_stride = MALI_ALIGN(hnd->byte_stride / 2, 16);
      int v_size = vu_stride * (adjusted_height / 2);
      /* V plane*/
      bo->gem_handles[1] = gem_handle;
      bo->pitches[1] = vu_stride;
      bo->offsets[1] = y_size;
      /* U plane */
      bo->gem_handles[2] = gem_handle;
      bo->pitches[2] = vu_stride;
      bo->offsets[2] = y_size + v_size;
      // ALOGI("~~~ bo->pitches[0]=%d", bo->pitches[0]);
      // ALOGI("~~~ bo->offsets[0]=%d", bo->offsets[0]);      
      // ALOGI("~~~ bo->pitches[1]=%d", bo->pitches[1]);
      // ALOGI("~~~ bo->offsets[1]=%d", bo->offsets[1]);
      // ALOGI("~~~ bo->pitches[2]=%d", bo->pitches[2]);
      // ALOGI("~~~ bo->offsets[2]=%d", bo->offsets[2]);
      // ALOGI("~~~ size=%d", hnd->size);
      break;
    }

    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21: {
      /* Y plane*/
      int adjusted_height = MALI_ALIGN(hnd->height, 2);
      int y_size = adjusted_height * hnd->byte_stride;

      /* U+V plane*/
      int vu_stride = MALI_ALIGN(hnd->byte_stride / 2, 16) * 2;

      bo->gem_handles[1] = gem_handle;
      bo->pitches[1] = vu_stride;
      bo->offsets[1] = y_size;
      // ALOGI("~~~ bo->pitches[0]=%d", bo->pitches[0]);
      // ALOGI("~~~ bo->offsets[0]=%d", bo->offsets[0]);      
      // ALOGI("~~~ bo->pitches[1]=%d", bo->pitches[1]);
      // ALOGI("~~~ bo->offsets[1]=%d", bo->offsets[1]);
      break;
    }
    default:
      break;
  }


  ret = drmModeAddFB2(drm_->fd(), bo->width, bo->height, bo->format,
                      bo->gem_handles, bo->pitches, bo->offsets, &bo->fb_id, 0);
  if (ret) {
    ALOGE("could not create drm fb %d", ret);
    return ret;
  }

  return ret;
}

bool PlanStageZynqMP::IsYuvHalFormat(uint32_t format){
  switch (format) {
    case HAL_PIXEL_FORMAT_YV12:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
      return true;
    default:
      return false;
  }
}

int PlanStageZynqMP::ProvisionPlanes(
    std::vector<DrmCompositionPlane> *composition,
    std::map<size_t, DrmHwcLayer *> &layers, DrmCrtc *crtc,
    std::vector<DrmPlane *> *planes) {
  int ret;

  // if we have only 1 layer, it should go to the primary plane
  ALOGI("~~~ got %zu layers", layers.size());
  // Go through YUV layers first
  for (auto i = layers.begin(); i != layers.end();) {

    private_handle_t const *hnd = reinterpret_cast < private_handle_t const *>(i->second->get_usable_handle());
    if (!hnd){
      ALOGW("Bad buffer handle");
      ++i;
      continue;
    }

    // if (!IsYuvHalFormat(hnd->format)) {
    //   ++i;
    //   continue;
    // }
    // ALOGI("~~~ FOUND YUV");
    int32_t drm_format = ConvertHalFormatToDrm(hnd->format);
    if (drm_format < 0){
      ++i;
      continue;
    }

    ret = EmplacePlaneByFormat(composition, planes, DrmCompositionPlane::Type::kLayer, crtc,
                  i->first, drm_format);
    // if (ret == -ENOENT)
    //   break;
    if (ret)
      ALOGE("Failed to emplace layer %zu, dropping it", i->first);

    i = layers.erase(i);
  }

  return 0;
}

uint32_t PlanStageZynqMP::ConvertHalFormatToDrm(uint32_t hal_format) {
  switch (hal_format) {
    case HAL_PIXEL_FORMAT_RGB_888:
      return DRM_FORMAT_BGR888;
    case HAL_PIXEL_FORMAT_BGRA_8888:
      return DRM_FORMAT_ARGB8888;
    case HAL_PIXEL_FORMAT_RGBX_8888:
      return DRM_FORMAT_XBGR8888;
    case HAL_PIXEL_FORMAT_RGBA_8888:
      return DRM_FORMAT_ABGR8888;
    case HAL_PIXEL_FORMAT_RGB_565:
      return DRM_FORMAT_BGR565;
    case HAL_PIXEL_FORMAT_YV12:
      return DRM_FORMAT_YVU420;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
      return DRM_FORMAT_NV12;
    default:
      ALOGE("Cannot convert hal format to drm format %u", hal_format);
      return -EINVAL;
  }
}

DrmPlane * PlanStageZynqMP::PopPlaneForFormat(std::vector<DrmPlane *> *planes, int32_t drm_format) {
  if (planes->empty())
    return NULL;
  DrmPlane *plane;
  
  for (auto p = planes->begin(); p != planes->end(); ++p) {
    plane = *p;
    if (plane->GetFormatSupported(drm_format)){
      ALOGI("<<< found plane=%d for format=%c%c%c%c", plane->id(),
        drm_format,
        drm_format >> 8,
        drm_format >> 16,
        drm_format >> 24);
      planes->erase(p);
      return plane;
    }
  }
  ALOGI("<<< didn't find plane for format=%c%c%c%c",
        drm_format,
        drm_format >> 8,
        drm_format >> 16,
        drm_format >> 24);
  return NULL;
}

// Inserts the given layer:plane in the composition at the back
int  PlanStageZynqMP::EmplacePlaneByFormat(std::vector<DrmCompositionPlane> *composition,
                   std::vector<DrmPlane *> *planes,
                   DrmCompositionPlane::Type type, DrmCrtc *crtc,
                   size_t source_layer,
                   int32_t drm_format) {
  DrmPlane *plane = PopPlaneForFormat(planes, drm_format);
  if (!plane)
    return -ENOENT;

  composition->emplace_back(type, plane, crtc, source_layer);
  return 0;
}

std::unique_ptr<Planner> Planner::CreateInstance(DrmResources *) {
  std::unique_ptr<Planner> planner(new Planner);
  planner->AddStage<PlanStageZynqMP>();
  //planner->AddStage<PlanStageGreedy>();
  return planner;
}
}
