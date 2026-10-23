/* The renderer entry points of render.h, listed once: render.c picks a
   backend at startup (RECOMP_RENDERER=d3d9 for the Direct3D 9 reference,
   otherwise Direct3D 11) and forwards each call to it. A backend source
   defines RENDER_BACKEND (d3d9, d3d11) before including this header, which
   renames its implementations to <backend>_r_... */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* R(return type, name, parameters, arguments); V for functions returning void */
#define RENDER_API(R, V)                                                                                              \
    R(int, r_init, (uint32_t width, uint32_t height), (width, height))                                                 \
    R(int, r_ready, (void), ())                                                                                        \
    R(int, r_has_focus, (void), ())                                                                                    \
    V(r_present, (void), ())                                                                                           \
    V(r_set_render_target, (int back_buffer, uint32_t w, uint32_t h, int z_kind, uint32_t z_data, uint32_t z_w, uint32_t z_h), \
      (back_buffer, w, h, z_kind, z_data, z_w, z_h))                                                                   \
    V(r_clear, (uint32_t xbox_flags, uint32_t color, float z, uint32_t stencil), (xbox_flags, color, z, stencil))      \
    V(r_set_rs_simple, (uint32_t method, uint32_t value), (method, value))                                             \
    V(r_set_rs_deferred, (uint32_t xbox_state, uint32_t value), (xbox_state, value))                                   \
    V(r_set_rs, (uint32_t pc_state, uint32_t value), (pc_state, value))                                                \
    V(r_set_tss, (uint32_t stage, uint32_t xbox_type, uint32_t value), (stage, xbox_type, value))                      \
    V(r_set_texture, (uint32_t stage, uint32_t guest_object), (stage, guest_object))                                   \
    V(r_set_transform, (uint32_t state, const float *m), (state, m))                                                   \
    V(r_set_viewport, (uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz), (x, y, w, h, minz, maxz)) \
    V(r_set_vertex_shader, (uint32_t handle), (handle))                                                                \
    V(r_create_vertex_shader, (uint32_t handle, const char *src, size_t len, const uint32_t *decl, int ndecl),         \
      (handle, src, len, decl, ndecl))                                                                                 \
    V(r_delete_vertex_shader, (uint32_t handle), (handle))                                                             \
    V(r_set_vs_constants, (int reg, const float *data, uint32_t count), (reg, data, count))                            \
    V(r_set_pixel_shader, (uint32_t handle), (handle))                                                                 \
    V(r_create_pixel_shader, (uint32_t handle, const uint32_t *psdef), (handle, psdef))                                \
    V(r_create_pixel_shader_source, (uint32_t handle, const char *src, size_t len), (handle, src, len))                \
    V(r_delete_pixel_shader, (uint32_t handle), (handle))                                                              \
    V(r_set_ps_constants, (int reg, const float *data, uint32_t count), (reg, data, count))                            \
    V(r_set_stream, (uint32_t stream, uint32_t vb_object, uint32_t stride), (stream, vb_object, stride))               \
    V(r_draw, (uint32_t prim, uint32_t start_vertex, uint32_t vertex_count), (prim, start_vertex, vertex_count))       \
    V(r_draw_up, (uint32_t prim, uint32_t vertex_count, uint32_t data, uint32_t stride), (prim, vertex_count, data, stride)) \
    V(r_draw_indexed, (uint32_t prim, uint32_t vertex_count, uint32_t index_data, uint32_t base_vertex),               \
      (prim, vertex_count, index_data, base_vertex))                                                                   \
    R(uint32_t, r_create_state_block, (void), ())                                                                      \
    V(r_capture_state_block, (uint32_t token), (token))                                                                \
    V(r_apply_state_block, (uint32_t token), (token))                                                                  \
    V(r_delete_state_block, (uint32_t token), (token))                                                                 \
    V(r_texture_dirty, (uint32_t guest_object), (guest_object))                                                        \
    V(r_resource_freed, (uint32_t guest_object), (guest_object))

#ifdef RENDER_BACKEND
#define RB_CAT2(a, b) a##_##b
#define RB_CAT(a, b) RB_CAT2(a, b)
#define r_init RB_CAT(RENDER_BACKEND, r_init)
#define r_ready RB_CAT(RENDER_BACKEND, r_ready)
#define r_has_focus RB_CAT(RENDER_BACKEND, r_has_focus)
#define r_present RB_CAT(RENDER_BACKEND, r_present)
#define r_set_render_target RB_CAT(RENDER_BACKEND, r_set_render_target)
#define r_clear RB_CAT(RENDER_BACKEND, r_clear)
#define r_set_rs_simple RB_CAT(RENDER_BACKEND, r_set_rs_simple)
#define r_set_rs_deferred RB_CAT(RENDER_BACKEND, r_set_rs_deferred)
#define r_set_rs RB_CAT(RENDER_BACKEND, r_set_rs)
#define r_set_tss RB_CAT(RENDER_BACKEND, r_set_tss)
#define r_set_texture RB_CAT(RENDER_BACKEND, r_set_texture)
#define r_set_transform RB_CAT(RENDER_BACKEND, r_set_transform)
#define r_set_viewport RB_CAT(RENDER_BACKEND, r_set_viewport)
#define r_set_vertex_shader RB_CAT(RENDER_BACKEND, r_set_vertex_shader)
#define r_create_vertex_shader RB_CAT(RENDER_BACKEND, r_create_vertex_shader)
#define r_delete_vertex_shader RB_CAT(RENDER_BACKEND, r_delete_vertex_shader)
#define r_set_vs_constants RB_CAT(RENDER_BACKEND, r_set_vs_constants)
#define r_set_pixel_shader RB_CAT(RENDER_BACKEND, r_set_pixel_shader)
#define r_create_pixel_shader RB_CAT(RENDER_BACKEND, r_create_pixel_shader)
#define r_create_pixel_shader_source RB_CAT(RENDER_BACKEND, r_create_pixel_shader_source)
#define r_delete_pixel_shader RB_CAT(RENDER_BACKEND, r_delete_pixel_shader)
#define r_set_ps_constants RB_CAT(RENDER_BACKEND, r_set_ps_constants)
#define r_set_stream RB_CAT(RENDER_BACKEND, r_set_stream)
#define r_draw RB_CAT(RENDER_BACKEND, r_draw)
#define r_draw_up RB_CAT(RENDER_BACKEND, r_draw_up)
#define r_draw_indexed RB_CAT(RENDER_BACKEND, r_draw_indexed)
#define r_create_state_block RB_CAT(RENDER_BACKEND, r_create_state_block)
#define r_capture_state_block RB_CAT(RENDER_BACKEND, r_capture_state_block)
#define r_apply_state_block RB_CAT(RENDER_BACKEND, r_apply_state_block)
#define r_delete_state_block RB_CAT(RENDER_BACKEND, r_delete_state_block)
#define r_texture_dirty RB_CAT(RENDER_BACKEND, r_texture_dirty)
#define r_resource_freed RB_CAT(RENDER_BACKEND, r_resource_freed)
#endif
