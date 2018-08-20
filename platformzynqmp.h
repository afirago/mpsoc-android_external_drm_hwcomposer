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

#ifndef ANDROID_PLATFORM_ZYNQMP_H_
#define ANDROID_PLATFORM_ZYNQMP_H_

#include "drmresources.h"
#include "platform.h"
#include "platformdrmgeneric.h"

#include <stdatomic.h>

#include <hardware/gralloc.h>

namespace android {

class ZynqmpImporter : public DrmGenericImporter {
 public:
  ZynqmpImporter(DrmResources *drm);
  ~ZynqmpImporter() override;

  int Init();

  int CheckBuffer(buffer_handle_t handle) override;
  int ImportBuffer(buffer_handle_t handle, hwc_drm_bo_t *bo) override;
  int IsRgbBuffer(buffer_handle_t handle) override;

  uint32_t ConvertHalFormatToDrm(uint32_t hal_format);
 private:

  DrmResources *drm_;

  const gralloc_module_t *gralloc_;
};

// TODO
class PlanStageZynqMP : public Planner::PlanStage {
 private:
  bool IsYuvHalFormat(uint32_t format);
 public:
  int ProvisionPlanes(std::vector<DrmCompositionPlane> *composition,
                      std::map<size_t, DrmHwcLayer *> &layers, DrmCrtc *crtc,
                      std::vector<DrmPlane *> *planes);
  uint32_t ConvertHalFormatToDrm(uint32_t hal_format);
   protected:
    // Removes and returns the next available YUV plane from planes
    DrmPlane *PopPlaneForFormat(std::vector<DrmPlane *> *planes, int32_t drm_format);

    // Inserts the given layer:plane in the composition at the back
    int EmplacePlaneByFormat(std::vector<DrmCompositionPlane> *composition,
                       std::vector<DrmPlane *> *planes,
                       DrmCompositionPlane::Type type, DrmCrtc *crtc,
                       size_t source_layer, int32_t drm_format);
};

}

#endif
