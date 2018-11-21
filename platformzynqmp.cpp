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

#include "platformzynqmp.h"
#include "drmdevice.h"
#include "platform.h"

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

Importer *Importer::CreateInstance(DrmDevice *drm) {
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

ZynqmpImporter::ZynqmpImporter(DrmDevice *drm) : DrmGenericImporter(drm), drm_(drm) {
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

static uint32_t ZynqmpConvertHalFormatToDrm(uint32_t hal_format) {
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
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
      return DRM_FORMAT_NV12;
    default:
      ALOGE("Cannot convert hal format to drm format %u", hal_format);
      return -EINVAL;
  }
}

/*
 * Check if we can export given buffer.
 * We can export buffers allocated by gralloc/ion from CMA heap.
 * This function should be in sync with gralloc
 */
bool ZynqmpImporter::CanImportBuffer(buffer_handle_t handle) {
  private_handle_t const *hnd = reinterpret_cast<private_handle_t const *>(
      handle);

  // Camera buffers were allocated from DMA/CMA heap, we can export
  if (hnd->usage & GRALLOC_USAGE_HW_CAMERA_WRITE) {
    return true;
  }

  // non-FB buffers were allocated from system heap, cannot export
  if (!(hnd->usage & GRALLOC_USAGE_HW_FB))
    return false;

  return true;
}

int ZynqmpImporter::ImportBuffer(buffer_handle_t handle, hwc_drm_bo_t *bo) {
  private_handle_t const *hnd = reinterpret_cast < private_handle_t const *>(handle);
  if (!hnd)
    return -EINVAL;

  /*
   * We can't import these types of buffers.
   * These buffers should be sent to client composition on validate display step
   * after checking with CanImportBuffer()
   */
  if (!(hnd->usage & GRALLOC_USAGE_HW_FB) && !(hnd->usage & GRALLOC_USAGE_HW_CAMERA_WRITE))
    return -EINVAL;

  uint32_t gem_handle;
  int ret = drmPrimeFDToHandle(drm_->fd(), hnd->share_fd, &gem_handle);
  if (ret) {
    ALOGE("failed to import prime fd %d ret=%d", hnd->share_fd, ret);
    return ret;
  }

  int32_t fmt = ZynqmpConvertHalFormatToDrm(hnd->format);
  if (fmt < 0)
    return fmt;

  memset(bo, 0, sizeof(hwc_drm_bo_t));
  bo->width = hnd->width;
  bo->height = hnd->height;
  bo->hal_format = hnd->format;
  bo->format = fmt;
  bo->usage = hnd->usage;
  bo->pixel_stride = hnd->stride;
  bo->pitches[0] = hnd->byte_stride;
  bo->gem_handles[0] = gem_handle;
  bo->offsets[0] = 0;

  switch (fmt) {
    case DRM_FORMAT_YVU420: {
      int align = 128;
      if (hnd->usage &
          (GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK))
        align = 16;
      int adjusted_height = MALI_ALIGN(hnd->height, 2);
      int y_size = adjusted_height * hnd->byte_stride;
      int vu_stride = MALI_ALIGN(hnd->byte_stride / 2, align);
      int v_size = vu_stride * (adjusted_height / 2);

      /* V plane*/
      bo->gem_handles[1] = gem_handle;
      bo->pitches[1] = vu_stride;
      bo->offsets[1] = y_size;
      /* U plane */
      bo->gem_handles[2] = gem_handle;
      bo->pitches[2] = vu_stride;
      bo->offsets[2] = y_size + v_size;
      break;
    }
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21: {
      /* Y plane*/
      int adjusted_height = MALI_ALIGN(hnd->height, 2);
      int y_size = adjusted_height * hnd->byte_stride;
      /* U+V plane */
      int vu_stride = MALI_ALIGN(hnd->byte_stride / 2, 16) * 2;
      bo->gem_handles[1] = gem_handle;
      bo->pitches[1] = vu_stride;
      bo->offsets[1] = y_size;
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

static bool IsRgbDrmFormat(uint32_t drm_format) {
  switch (drm_format) {
    case DRM_FORMAT_BGR888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_BGR565:
      return true;
    default:
      return false;
  }
}

class PlanStageZynqmp : public Planner::PlanStage {
 private:
    // Find the next available plane which supports given format and remove it from planes
    static DrmPlane *PopPlaneForFormat(std::vector<DrmPlane *> *planes, uint32_t format) {
      if (planes->empty())
        return NULL;

      // FIXME:
      // hack: currently RGB formats are only supported by the primary plane
      if (IsRgbDrmFormat(format)) {
        for (size_t i = 0; i != planes->size(); ++i) {
          if (planes->at(i)->type() == DRM_PLANE_TYPE_PRIMARY){
            DrmPlane *plane = planes->at(i);
            planes->erase(planes->begin() + i);
            return plane;
          }
        }
        return NULL;
      }

      for (size_t i = 0; i != planes->size(); ++i) {
        if (planes->at(i)->GetFormatSupported(format)) {
          DrmPlane *plane = planes->at(i);
          planes->erase(planes->begin() + i);
          return plane;
        }
      }
      return NULL;
    }

    // Tries to find a plane supporting specified format for a layer
    // If found, inserts plane:layer at the composition at the back
    static int EmplaceForFormat(std::vector<DrmCompositionPlane> *composition,
                       std::vector<DrmPlane *> *planes,
                       DrmCompositionPlane::Type type, DrmCrtc *crtc,
                       size_t source_layer, uint32_t format) {
      DrmPlane *plane = PopPlaneForFormat(planes, format);
      if (!plane)
        return -ENOENT;

      composition->emplace_back(type, plane, crtc, source_layer);
      return 0;
    }

 public:
  int ProvisionPlanes(std::vector<DrmCompositionPlane> *composition,
                      std::map<size_t, DrmHwcLayer *> &layers, DrmCrtc *crtc,
                      std::vector<DrmPlane *> *planes) {
    int layers_added = 0;
    int initial_layers = layers.size();

    (void)crtc;
    (void)planes;
    (void)composition;

    for (auto i = layers.begin(); i != layers.end(); i = layers.erase(i)) {
      private_handle_t const *hnd = reinterpret_cast < private_handle_t const *>(i->second->get_usable_handle());
      if (!hnd){
        ALOGW("Bad buffer handle");
        continue;
      }

      int32_t drm_format = ZynqmpConvertHalFormatToDrm(hnd->format);
      if (drm_format < 0) {
        ALOGW("Bad buffer format");
        continue;
      }

      int ret = EmplaceForFormat(composition, planes, DrmCompositionPlane::Type::kLayer,
                        crtc, i->first, drm_format);
      layers_added++;
      // We don't have any planes left
      if (ret == -ENOENT)
        break;
      else if (ret) {
        ALOGE("Failed to emplace layer %zu, dropping it", i->first);
        return ret;
      }
    }
    /*
     * If we only have one layer, but we didn't emplace anything, we
     * can run into trouble, as we might try to device composite a
     * buffer we fake-imported, which can cause things to jamb up.
     * So return an error in this case to ensure we force client
     * compositing.
     */
    if (!layers_added && (initial_layers <= 1))
      return -EINVAL;

    return 0;
  }
};

std::unique_ptr<Planner> Planner::CreateInstance(DrmDevice *) {
  std::unique_ptr<Planner> planner(new Planner);
  planner->AddStage<PlanStageZynqmp>();
  return planner;
}
} //namespace android
