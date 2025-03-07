/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_shader_private.hh"

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_push_constants.hh"

#include "BLI_string_ref.hh"

namespace blender::gpu {
class VKShaderInterface;

class VKShader : public Shader {
 private:
  VKContext *context_ = nullptr;
  VkShaderModule vertex_module_ = VK_NULL_HANDLE;
  VkShaderModule geometry_module_ = VK_NULL_HANDLE;
  VkShaderModule fragment_module_ = VK_NULL_HANDLE;
  VkShaderModule compute_module_ = VK_NULL_HANDLE;
  bool compilation_failed_ = false;

  /**
   * Not owning handle to the descriptor layout.
   * The handle is owned by `VKDescriptorSetLayouts` of the device.
   */
  VkDescriptorSetLayout vk_descriptor_set_layout_ = VK_NULL_HANDLE;
  /* deprecated `when use_render_graph=true`. In that case use vk_pipeline_ */

  /**
   * Last created VkPipeline handle. This handle is used as template when building a variation of
   * the shader. In case for compute shaders without specialization constants this handle is also
   * used as an early exit. In this case there is only 1 variation.
   */
  /* TODO: Should be refactored to store the `vk_pipeline_base_`.
   * What is the reason to store the last pipeline. */
  VkPipeline vk_pipeline_ = VK_NULL_HANDLE;

  bool is_static_shader_ = false;

 public:
  VkPipelineLayout vk_pipeline_layout = VK_NULL_HANDLE;
  VKPushConstants push_constants;

  VKShader(const char *name);
  virtual ~VKShader();

  void init(const shader::ShaderCreateInfo &info, bool is_batch_compilation) override;

  void vertex_shader_from_glsl(MutableSpan<const char *> sources) override;
  void geometry_shader_from_glsl(MutableSpan<const char *> sources) override;
  void fragment_shader_from_glsl(MutableSpan<const char *> sources) override;
  void compute_shader_from_glsl(MutableSpan<const char *> sources) override;
  bool finalize(const shader::ShaderCreateInfo *info = nullptr) override;
  void warm_cache(int limit) override;

  void transform_feedback_names_set(Span<const char *> name_list,
                                    eGPUShaderTFBType geom_type) override;
  bool transform_feedback_enable(VertBuf *) override;
  void transform_feedback_disable() override;

  void bind() override;
  void unbind() override;

  void uniform_float(int location, int comp_len, int array_size, const float *data) override;
  void uniform_int(int location, int comp_len, int array_size, const int *data) override;

  std::string resources_declare(const shader::ShaderCreateInfo &info) const override;
  std::string vertex_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string fragment_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string geometry_interface_declare(const shader::ShaderCreateInfo &info) const override;
  std::string geometry_layout_declare(const shader::ShaderCreateInfo &info) const override;
  std::string compute_layout_declare(const shader::ShaderCreateInfo &info) const override;

  /* Unused: SSBO vertex fetch draw parameters. */
  bool get_uses_ssbo_vertex_fetch() const override
  {
    return false;
  }
  int get_ssbo_vertex_fetch_output_num_verts() const override
  {
    return 0;
  }

  /* DEPRECATED: Kept only because of BGL API. */
  int program_handle_get() const override;

  VkPipeline ensure_and_get_compute_pipeline();
  VkPipeline ensure_and_get_graphics_pipeline(GPUPrimType primitive,
                                              VKVertexAttributeObject &vao,
                                              VKStateManager &state_manager,
                                              VKFrameBuffer &framebuffer);

  const VKShaderInterface &interface_get() const;

  bool is_graphics_shader() const
  {
    return !is_compute_shader();
  }

  bool is_compute_shader() const
  {
    return compute_module_ != VK_NULL_HANDLE;
  }

  /**
   * Some shaders don't have a descriptor set and should not bind any descriptor set to the
   * pipeline. This function can be used to determine if a descriptor set can be bound when this
   * shader or one of its pipelines are active.
   */
  bool has_descriptor_set() const
  {
    return vk_descriptor_set_layout_ != VK_NULL_HANDLE;
  }

  VkDescriptorSetLayout vk_descriptor_set_layout_get() const
  {
    return vk_descriptor_set_layout_;
  }

 private:
  Vector<uint32_t> compile_glsl_to_spirv(Span<const char *> sources, shaderc_shader_kind kind);
  void build_shader_module(Span<uint32_t> spirv_module, VkShaderModule *r_shader_module);
  void build_shader_module(MutableSpan<const char *> sources,
                           shaderc_shader_kind stage,
                           VkShaderModule *r_shader_module);
  bool finalize_descriptor_set_layouts(VKDevice &vk_device,
                                       const VKShaderInterface &shader_interface);
  bool finalize_pipeline_layout(VkDevice vk_device, const VKShaderInterface &shader_interface);

  /**
   * \brief features available on newer implementation such as native barycentric coordinates
   * and layered rendering, necessitate a geometry shader to work on older hardware.
   */
  std::string workaround_geometry_shader_source_create(const shader::ShaderCreateInfo &info);
  bool do_geometry_shader_injection(const shader::ShaderCreateInfo *info);
};

static inline VKShader &unwrap(Shader &shader)
{
  return static_cast<VKShader &>(shader);
}

static inline VKShader *unwrap(Shader *shader)
{
  return static_cast<VKShader *>(shader);
}

}  // namespace blender::gpu
