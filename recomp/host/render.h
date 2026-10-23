/* Host renderer behind the Xbox D3D8 hooks (hooks_d3d.c). The hooks keep the
   guest-visible resource objects; this layer turns their contents and the
   device state into draw calls on a host API (Direct3D 9 for now: it still
   has the fixed-function pipeline the Xbox API maps onto almost one to one). */
#pragma once
#include <stddef.h>
#include <stdint.h>

int  r_init(uint32_t width, uint32_t height);
int  r_ready(void); /* a device exists */
int  r_has_focus(void); /* the game window is the foreground window */
void r_present(void);
void r_clear(uint32_t xbox_flags, uint32_t color, float z, uint32_t stencil);

/* Xbox D3DDevice_SetRenderState_Simple: an NV2A method address and the value
   in the console's encoding; _Deferred: an Xbox D3DRS index with a D3D value;
   the dedicated setters pass a PC D3DRS number through r_set_rs. */
void r_set_rs_simple(uint32_t method, uint32_t value);
void r_set_rs_deferred(uint32_t xbox_state, uint32_t value);
void r_set_rs(uint32_t pc_state, uint32_t value);
void r_set_tss(uint32_t stage, uint32_t xbox_type, uint32_t value);
void r_set_texture(uint32_t stage, uint32_t guest_object);
void r_set_transform(uint32_t state, const float *m);
void r_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz);
void r_set_vertex_shader(uint32_t handle);
/* programmable vertex shaders: the game's source text (translated by
   shader_xlate.c) and the Xbox vertex declaration tokens, up to 0xFFFFFFFF */
void r_create_vertex_shader(uint32_t handle, const char *src, size_t len, const uint32_t *decl, int ndecl);
void r_delete_vertex_shader(uint32_t handle);
void r_set_vs_constants(int reg, const float *data, uint32_t count); /* c0..c95 */
void r_set_pixel_shader(uint32_t handle);
void r_create_pixel_shader(uint32_t handle, const uint32_t *psdef); /* D3DPIXELSHADERDEF, 66 dwords */
/* a pixel shader from the game's source text (shader_xlate.c) */
void r_create_pixel_shader_source(uint32_t handle, const char *src, size_t len);
void r_delete_pixel_shader(uint32_t handle);
void r_set_ps_constants(int reg, const float *data, uint32_t count); /* c0..c7 */
void r_set_stream(uint32_t stream, uint32_t vb_object, uint32_t stride);
void r_draw(uint32_t prim, uint32_t start_vertex, uint32_t vertex_count);
void r_draw_up(uint32_t prim, uint32_t vertex_count, uint32_t data, uint32_t stride);
void r_draw_indexed(uint32_t prim, uint32_t vertex_count, uint32_t index_data, uint32_t base_vertex);

/* D3DSBT_ALL state blocks: the whole device state, captured at creation and
   by Capture, restored by Apply. Tokens are never 0. */
uint32_t r_create_state_block(void);
void r_capture_state_block(uint32_t token);
void r_apply_state_block(uint32_t token);
void r_delete_state_block(uint32_t token);

/* the guest wrote to a pixel container (Lock, Register) or freed it */
void r_texture_dirty(uint32_t guest_object);
void r_resource_freed(uint32_t guest_object);

/* hooks_d3d.c: dimensions of a guest pixel container level */
void d3d_describe(uint32_t object, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *pitch);
uint32_t d3d_level_offset(uint32_t object, uint32_t level);
int d3d_has_linear_layout(uint32_t object); /* rows at a given pitch regardless of the format code */
