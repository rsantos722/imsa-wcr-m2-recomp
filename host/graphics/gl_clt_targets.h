// Guest-addressed color storage shared by the two OpenGL CLT backends.
// State 3 draws to the Z bitmap's address after drawing the world: those
// writes must not alias the world's color bytes. Depth remains shared, as in
// the existing backends. The near-cell color alias uses per-sample stencil
// coverage below to apply its color-output relation before updating depth.
#pragma once

#include "cel_blitter.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <vector>

// Core-profile headers do not have to expose the multisample query token.
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif

namespace m2host {

class GlCltTargets {
public:
    bool init()
    {
#define LOAD(name) name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(#name)); if (!name) return false
        LOAD(glGenFramebuffers); LOAD(glDeleteFramebuffers);
        LOAD(glBindFramebuffer); LOAD(glCheckFramebufferStatus);
        LOAD(glFramebufferRenderbuffer); LOAD(glFramebufferTexture2D);
        LOAD(glBlitFramebuffer);
        LOAD(glGenRenderbuffers); LOAD(glDeleteRenderbuffers);
        LOAD(glBindRenderbuffer); LOAD(glRenderbufferStorage);
        LOAD(glRenderbufferStorageMultisample);
        LOAD(glClearBufferfv); LOAD(glClearBufferiv);
        LOAD(glStencilFunc); LOAD(glStencilOp); LOAD(glStencilMask);
        LOAD(glDepthRange); LOAD(glDrawBuffers); LOAD(glDisablei);
#undef LOAD
        return true;
    }

    void metadata_write_state() { glDisablei(GL_BLEND, 1); }

    void clear_metadata(float alpha = 1.f) {
        const GLfloat value[4] = {0,alpha,0,1};
        glClearBufferfv(GL_COLOR, 1, value);
    }
    void metadata_upload_output(bool enabled) {
        const GLenum outputs[] = {static_cast<GLenum>(enabled ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0), GL_COLOR_ATTACHMENT1};
        glDrawBuffers(enabled ? 1 : 2, outputs);
    }

    void shutdown()
    {
        if (!glBindFramebuffer) return;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for (auto& [base, target] : m_targets) {
            glDeleteFramebuffers(1, &target.fbo);
            glDeleteRenderbuffers(1, &target.color);
            glDeleteRenderbuffers(1, &target.ssb);
        }
        m_targets.clear();
        if (m_snapshot_fbo) glDeleteFramebuffers(1, &m_snapshot_fbo);
        m_snapshot_fbo = 0;
        if (m_resolve_fbo) glDeleteFramebuffers(1, &m_resolve_fbo);
        m_resolve_fbo = 0;
        if (m_resolve_rbo) glDeleteRenderbuffers(1, &m_resolve_rbo);
        m_resolve_rbo = 0;
        m_resolve_w = m_resolve_h = 0;
        for (ScaleBuffer& buffer : m_scale_buffers) release_scale_buffer(buffer);
        m_scale_buffers.clear();
        if (m_depth) glDeleteRenderbuffers(1, &m_depth);
        m_depth = 0;
        m_base = 0;
        m_present_base = 0;
    }

    // Highest MSAA sample count this context can use for renderbuffers
    // (1 when the entry point or the device cannot serve multisampling).
    int usable_samples() const
    {
        if (!glRenderbufferStorageMultisample) return 1;
        GLint max_samples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        return max_samples >= 2 ? static_cast<int>(max_samples) : 1;
    }

    // Sample count for storage created afterwards. The backends set this once
    // at init, before the first addressed target exists; it does not convert
    // storage that is already allocated.
    void set_samples(int samples) { m_samples = samples < 1 ? 1 : samples; }
    int samples() const { return m_samples; }

    // Called at the normal host frame boundary; resize invalidates storage.
    // The backend retains its existing frame clear policy after this call.
    void resize(int w, int h, float r, float g, float b)
    {
        m_clear[0] = r; m_clear[1] = g; m_clear[2] = b;
        if (w == m_w && h == m_h) {
            bind_active();
            return;
        }
        const uint32_t base = m_base;
        shutdown();
        m_w = w; m_h = h;
        select(base);
        bind_active();
    }

    // Depth value a newly created depth attachment starts from.  The backends
    // set this to the guest Z plane's far value (`kCltTeZFarDepth`); the
    // guest's own TE clear-Z request then supplies the authored fill for the
    // frame.  It is deliberately not read from GL's clear-depth state, which
    // depends on call ordering and would silently invert the plane.
    void set_depth_clear(float value) { m_depth_clear = value; }

    // Clear the active target within the caller's scissor box. The recovered
    // TE driver serves TE_CLEAR_FRAME_BUFFER with its generated clear list, a
    // single untextured quad from the surface origin to the extent the bound
    // bitmap published at SETFRAMEBUFFER/SETZBUFFER (driver.c:251-278). That
    // quad is clipped by DBFBCLIP/window state exactly like a guest primitive,
    // so a clear must not escape the color-write clip the caller (the
    // interpreter's destination/window scissor) has already established.
    // Forcing a full-target clear here is what cleared the display border when
    // the guest asked for a clear on a surface smaller than the display.
    void clear_color(float r, float g, float b, float a)
    {
        if (!glClearBufferfv || m_base == 0) return;
        const GLfloat color[4] = {r, g, b, a};
        glClearBufferfv(GL_COLOR, 0, color);
        clear_metadata(a);
    }

    // Preserve the original pixel-depth predicate while writing a different
    // depth value through the color alias. Stencil records surviving samples
    // (including MSAA/discard/scissor), not a rectangle or triangle footprint.
    void begin_color_z_coverage()
    {
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0xff);
        const GLint zero = 0;
        glClearBufferiv(GL_STENCIL, 0, &zero);
        glStencilFunc(GL_ALWAYS, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }
    void resolve_color_z_coverage()
    {
        glStencilMask(0);
        glStencilFunc(GL_EQUAL, 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glDepthRange(kCltNearColorZ, kCltNearColorZ);
    }
    void end_color_z_coverage()
    {
        glDepthRange(0.0, 1.0);
        glStencilMask(0xff);
        glDisable(GL_STENCIL_TEST);
    }

    void select(uint32_t base)
    {
        if (base == 0) return;
        // present() leaves the default drawable bound. A later in-flight
        // guest list can legitimately keep the same destination base, so
        // selecting that base must rebind its FBO even when the identity did
        // not change.
        if (base == m_base) {
            touch(base);
            bind_active();
            return;
        }
        if (m_w <= 0 || m_h <= 0) return;
        auto found = m_targets.find(base);
        if (found == m_targets.end()) {
            // A valid guest stream may name more distinct DBDESTBASEADDR
            // addresses than the pool holds; the bridge's own span table
            // releases rather than fails for the same reason. Exceeding this
            // host memory bound releases a target instead of terminating the
            // process, and the release is counted and logged.
            if (m_targets.size() >= kMaxTargets && !release_for(base)) {
                ++m_refusals;
                report_capacity("refusal", base);
                return;
            }
            Target target;
            glGenFramebuffers(1, &target.fbo);
            glGenRenderbuffers(1, &target.color);
            glBindRenderbuffer(GL_RENDERBUFFER, target.color);
            if (m_samples > 1)
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples,
                                                 GL_RGBA8, m_w, m_h);
            else
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_w, m_h);
            glGenRenderbuffers(1, &target.ssb);
            glBindRenderbuffer(GL_RENDERBUFFER, target.ssb);
            if (m_samples > 1)
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8, m_w, m_h);
            else glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_w, m_h);
            const bool new_depth = m_depth == 0;
            if (new_depth) {
                glGenRenderbuffers(1, &m_depth);
                glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
                if (m_samples > 1)
                    glRenderbufferStorageMultisample(
                        GL_RENDERBUFFER, m_samples, GL_DEPTH24_STENCIL8, m_w,
                        m_h);
                else
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                          m_w, m_h);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_RENDERBUFFER, target.color);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, target.ssb);
            const GLenum outputs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(2, outputs);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      GL_DEPTH_STENCIL_ATTACHMENT,
                                      GL_RENDERBUFFER, m_depth);
            found = m_targets.emplace(base, target).first;
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("CLT framebuffer incomplete");
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            glDisable(GL_SCISSOR_TEST);
            glClearBufferfv(GL_COLOR, 0, m_clear);
            clear_metadata();
            if (new_depth) {
                glClearBufferfv(GL_DEPTH, 0, &m_depth_clear);
            }
            if (scissor) glEnable(GL_SCISSOR_TEST);
        }
        touch(base);
        m_base = base;
        glBindFramebuffer(GL_FRAMEBUFFER, found->second.fbo);
    }

    // Bounded-pool accounting. `releases()` counts addressed targets the
    // host gave back to make room; `refusals()` counts a request no victim
    // could satisfy. Neither is fatal, and both stay non-zero in a run log.
    size_t pool_size() const { return m_targets.size(); }
    uint64_t releases() const { return m_releases; }
    uint64_t refusals() const { return m_refusals; }
    bool pooled(uint32_t base) const { return m_targets.count(base) != 0; }

    void select_scanout(uint32_t base) { m_present_base = base; }
    void set_scanout_rect(uint32_t base, const CltScanoutRect& rect) {
        const auto found = m_targets.find(base);
        if (found != m_targets.end()) found->second.scanout_rect = rect;
    }
    uint32_t scanout_base() const { return m_present_base; }
    uint32_t active_base() const { return m_base; }

    bool copy(uint32_t source_base, uint32_t dest_base)
    {
        const auto source = m_targets.find(source_base);
        const auto dest = m_targets.find(dest_base);
        if (source == m_targets.end() || dest == m_targets.end()) return false;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source->second.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dest->second.fbo);
        for (GLenum attachment : {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}) {
            glReadBuffer(attachment);
            glDrawBuffer(attachment);
            glBlitFramebuffer(0, 0, m_w, m_h, 0, 0, m_w, m_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        const GLenum outputs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, outputs);
        bind_active();
        return true;
    }

    // The source is a renderbuffer, so copy it into an independent texture
    // before a DB-source draw reads it. Flip the source Y interval: uploaded
    // CPU cels store their top row in GL texture row zero, and this GPU copy
    // must have the same UV orientation. The destination remains detached
    // between copies and can be sampled while later draws write the source.
    // A multisample source resolves into the single-sample plane first when
    // the destination scales: a resolve blit may not change rectangle extents.
    bool snapshot_to_texture(uint32_t base, GLuint texture, int x, int y,
                             int w, int h, int tex_w, int tex_h, bool ssb = false)
    {
        const auto source = m_targets.find(base);
        if (source == m_targets.end() || !texture || w <= 0 || h <= 0 ||
            tex_w <= 0 || tex_h <= 0 || x < 0 || y < 0 ||
            x + w > m_w || y + h > m_h)
            return false;
        if (!m_snapshot_fbo) glGenFramebuffers(1, &m_snapshot_fbo);
        if (!m_snapshot_fbo) return false;
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture, 0);
        bool complete =
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            GLuint read_fbo = source->second.fbo;
            // Bottom-up source interval of the top-left-addressed window.
            int sx0 = x, sy0 = m_h - y, sx1 = x + w, sy1 = m_h - y - h;
            if (m_samples > 1 && (w != tex_w || h != tex_h)) {
                if (!resolve_window(base, x, y, w, h, ssb)) {
                    complete = false;
                } else {
                    // The resolve already flipped the window into texture
                    // orientation, so the scaled copy is a straight blit.
                    read_fbo = m_resolve_fbo;
                    sx0 = 0;
                    sy0 = 0;
                    sx1 = w;
                    sy1 = h;
                }
            }
            if (complete) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
                glReadBuffer(ssb && read_fbo == source->second.fbo ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot_fbo);
                glBlitFramebuffer(sx0, sy0, sx1, sy1, 0, 0, tex_w, tex_h,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source->second.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_snapshot_fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, 0, 0);
        bind_active();
        if (scissor) glEnable(GL_SCISSOR_TEST);
        return complete;
    }

    // Single-sample resolve plane for multisample targets. GL requires a
    // multisample resolve blit to have identical source and destination
    // rectangles, and glReadPixels from a multisample framebuffer is invalid,
    // so every readback and any scaled snapshot resolves into this plane
    // first. One plane serves all targets and regions; it is bounded by the
    // target size and re-created after a resize.
    bool ensure_resolve()
    {
        if (m_samples <= 1) return false;
        if (m_resolve_fbo && m_resolve_w == m_w && m_resolve_h == m_h)
            return true;
        if (m_resolve_fbo) glDeleteFramebuffers(1, &m_resolve_fbo);
        if (m_resolve_rbo) glDeleteRenderbuffers(1, &m_resolve_rbo);
        m_resolve_fbo = m_resolve_rbo = 0;
        m_resolve_w = m_resolve_h = 0;
        glGenFramebuffers(1, &m_resolve_fbo);
        glGenRenderbuffers(1, &m_resolve_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_resolve_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, m_w, m_h);
        glBindFramebuffer(GL_FRAMEBUFFER, m_resolve_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, m_resolve_rbo);
        const bool complete =
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (complete) {
            m_resolve_w = m_w;
            m_resolve_h = m_h;
        } else {
            glDeleteFramebuffers(1, &m_resolve_fbo);
            glDeleteRenderbuffers(1, &m_resolve_rbo);
            m_resolve_fbo = m_resolve_rbo = 0;
        }
        bind_active();
        return complete;
    }

    // Resolve the addressed target's full extent into the single-sample plane.
    // Returns the framebuffer to read from (the target itself when it is not
    // multisampled, otherwise the resolve plane).
    GLuint resolved_read_fbo(uint32_t base)
    {
        const auto found = m_targets.find(base);
        if (found == m_targets.end()) return 0;
        if (m_samples <= 1) return found->second.fbo;
        if (!ensure_resolve()) return 0;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, found->second.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve_fbo);
        glBlitFramebuffer(0, 0, m_w, m_h, 0, 0, m_w, m_h, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
        return m_resolve_fbo;
    }

    // Resolve a top-left-origin window 1:1 into the resolve plane at (0,0).
    // Identical source/destination extents are what makes a multisample
    // resolve legal; scaling stays single-sample work.
    bool resolve_window(uint32_t base, int x, int y, int w, int h, bool ssb = false)
    {
        const auto found = m_targets.find(base);
        if (found == m_targets.end() || m_samples <= 1 || !ensure_resolve())
            return false;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, found->second.fbo);
        glReadBuffer(ssb ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve_fbo);
        glBlitFramebuffer(x, m_h - y, x + w, m_h - y - h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        return true;
    }

    bool read(uint32_t base, uint32_t* dst, int w, int h)
    {
        const auto found = m_targets.find(base);
        if (!dst || found == m_targets.end() || w != m_w || h != m_h)
            return false;
        GLint previous_read = 0, previous_draw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
        const GLuint read_fbo = resolved_read_fbo(base);
        if (read_fbo == 0) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(previous_read));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(previous_draw));
            return false;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        while (glGetError() != GL_NO_ERROR) {}
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        const bool ok = glGetError() == GL_NO_ERROR;
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previous_read));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previous_draw));
        if (!ok) return false;
        for (int y = 0; y < h / 2; ++y)
            std::swap_ranges(dst + static_cast<size_t>(y) * w,
                             dst + static_cast<size_t>(y + 1) * w,
                             dst + static_cast<size_t>(h - 1 - y) * w);
        return true;
    }

    bool read_presented(uint32_t base, uint32_t* dst, int w, int h)
    {
        if (!read(base, dst, w, h)) return false;
        m_targets.at(base).scanout_rect.apply(dst, w, h);
        return true;
    }

    // Read only a top-left-origin window from an addressed target. The
    // display-carve DB-source path normally needs 576x432 from a 640x480
    // target; keeping the transfer windowed avoids moving the unused border
    // through the GL/Metal bridge while preserving the same pixel packing.
    bool read_window(uint32_t base, uint32_t* dst, int x, int y, int w, int h, bool metadata = false)
    {
        const auto found = m_targets.find(base);
        if (!dst || found == m_targets.end() || w <= 0 || h <= 0 ||
            x < 0 || y < 0 || x + w > m_w || y + h > m_h)
            return false;
        GLint previous_read = 0, previous_draw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
        if (m_samples > 1 && !resolve_window(base, x, y, w, h, metadata)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(previous_read));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(previous_draw));
            return false;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          m_samples > 1 ? m_resolve_fbo : found->second.fbo);
        glReadBuffer(metadata && m_samples <= 1 ? GL_COLOR_ATTACHMENT1 : GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        while (glGetError() != GL_NO_ERROR) {}
        glReadPixels(m_samples > 1 ? 0 : x, m_samples > 1 ? 0 : m_h - y - h, w,
                     h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        const bool ok = glGetError() == GL_NO_ERROR;
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,
                          static_cast<GLuint>(previous_read));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                          static_cast<GLuint>(previous_draw));
        if (!ok) return false;
        // resolve_window already reverses the source Y interval. Its readback
        // is top-left ordered; only direct, single-sample reads need a flip.
        for (int row = 0; m_samples <= 1 && row < h / 2; ++row)
            std::swap_ranges(dst + static_cast<size_t>(row) * w,
                             dst + static_cast<size_t>(row + 1) * w,
                             dst + static_cast<size_t>(h - 1 - row) * w);
        return true;
    }

    // Default-framebuffer origin (GL bottom-left) at which present() places
    // the image region. Zero unless a fullscreen drawable is letterboxed.
    void set_present_origin(int x, int y) { m_present_x = x; m_present_y = y; }

    // Size of the image region on the drawable. Zero (the default) or a size
    // equal to the targets presents them 1:1, as when the targets follow the
    // window. A different size (M2_RENDER_SCALE) scales the finished image:
    // whole 2:1 linear steps (an exact 2x2 box) while the image is at least
    // twice the region, a nearest integer prescale when it is at most half,
    // and one final linear blit to the region.
    void set_present_size(int w, int h) { m_present_w = w; m_present_h = h; }

    // Present only the guest viewport carve (M2_CROP_OVERSCAN), scaled to fill
    // the m_w x m_h target region. Presentation only: stored targets, guest
    // memory readback and captures keep the full authored frame.
    void set_crop_overscan(bool enabled) { m_crop_overscan = enabled; }

    void present(bool copy_color = true)
    {
        const uint32_t present_base = m_present_base != 0 ? m_present_base : m_base;
        const auto found = m_targets.find(present_base);
        if (found == m_targets.end()) return;
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        if (m_present_x != 0 || m_present_y != 0) {
            // Black bars around a letterboxed fullscreen image. The target
            // blit (or the bitmap scanout quad) only covers the 4:3 region.
            const GLfloat black[4] = {0.f, 0.f, 0.f, 1.f};
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glClearBufferfv(GL_COLOR, 0, black);
        }
        const int pw = m_present_w > 0 ? m_present_w : m_w;
        const int ph = m_present_h > 0 ? m_present_h : m_h;
        if (copy_color && pw == m_w && ph == m_h) {
            // A fullscreen drawable larger than the 4:3 target places the
            // target at (m_present_x, m_present_y); the bars stay black.
            compose(found->second, present_base, 0, m_present_x, m_present_y);
        } else if (copy_color) {
            present_scaled(found->second, present_base, pw, ph);
        }
        // SDL's Cocoa swap path presents the drawable only while the default
        // framebuffer is bound. When a direct Projector scanout is active the
        // target copy is intentionally omitted because the full-screen
        // scanout quad covers it; still bind the drawable for the quad.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }

private:
    // Host memory bound, not a source-derived guest limit. The guest can own
    // up to the recovered item pool (GK_MAX_ITEMS = 256) distinct framebuffer
    // items, and each pooled address costs a full m_w*m_h RGBA8 renderbuffer,
    // so the pool stays at a size the host can hold and the overflow path is a
    // counted release rather than a larger allocation.
    static constexpr size_t kMaxTargets = 16;

    struct Target {
        GLuint fbo = 0, color = 0, ssb = 0;
        uint64_t last_use = 0;
        CltScanoutRect scanout_rect;
    };

    // The presented image at target resolution, drawn into `draw_fbo` with
    // its origin at (ox, oy): the overscan crop, or the whole target plus any
    // Projector expansion of its scanout rectangle.
    void compose(const Target& target, uint32_t present_base, GLuint draw_fbo,
                 GLint ox, GLint oy)
    {
        const auto& rect = target.scanout_rect;
        int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
        const bool crop =
            m_crop_overscan &&
            rect.crop_region(m_w, m_h, crop_x, crop_y, crop_w, crop_h);
        // A multisample resolve cannot scale. Resolve first, then perform
        // Projector expansion on the resulting single-sample image.
        const GLuint source = (crop || rect.scales(m_w, m_h))
                                  ? resolved_read_fbo(present_base)
                                  : target.fbo;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        if (crop) {
            // The carve's source pixels fill the whole target region. The
            // scale is not an integer, so filter rather than repeat rows.
            glBlitFramebuffer(crop_x, m_h - crop_y - crop_h,
                              crop_x + crop_w, m_h - crop_y,
                              ox, oy, ox + m_w, oy + m_h,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
            return;
        }
        glBlitFramebuffer(0, 0, m_w, m_h, ox, oy, ox + m_w, oy + m_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (rect.scales(m_w, m_h)) {
            glBlitFramebuffer(rect.x, m_h - rect.y - rect.source_height,
                              rect.x + rect.source_width, m_h - rect.y,
                              ox + rect.x,
                              oy + m_h - rect.y - rect.height,
                              ox + rect.x + rect.width,
                              oy + m_h - rect.y,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }

    // Scale the presented image from target resolution into the pw x ph
    // region at the present origin. The overscan crop reads the carve
    // straight from the target so the image is filtered once; a Projector
    // expansion is first composed at target resolution.
    void present_scaled(const Target& target, uint32_t present_base, int pw,
                        int ph)
    {
        const auto& rect = target.scanout_rect;
        int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
        GLuint src = 0;
        int sx = 0, sy = 0, sw = m_w, sh = m_h;
        if (m_crop_overscan &&
            rect.crop_region(m_w, m_h, crop_x, crop_y, crop_w, crop_h)) {
            src = resolved_read_fbo(present_base);
            sx = crop_x;
            sy = m_h - crop_y - crop_h;
            sw = crop_w;
            sh = crop_h;
        } else if (rect.scales(m_w, m_h) || m_samples > 1) {
            const ScaleBuffer* composed = scale_buffer(0, m_w, m_h);
            if (!composed) return;
            compose(target, present_base, composed->fbo, 0, 0);
            src = composed->fbo;
        } else {
            src = target.fbo;
        }
        if (src == 0) return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        size_t next = 1;
        // Downscale: each whole 2:1 linear step samples exactly between four
        // source pixels, a box filter; stop once one more step would undershoot.
        while (sw / 2 >= pw && sh / 2 >= ph) {
            const ScaleBuffer* half = scale_buffer(next++, sw / 2, sh / 2);
            if (!half) break;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, half->fbo);
            glBlitFramebuffer(sx, sy, sx + sw, sy + sh, 0, 0, half->w, half->h,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
            src = half->fbo;
            sx = sy = 0;
            sw = half->w;
            sh = half->h;
        }
        // Upscale: repeat whole pixels first so the final linear blit only
        // softens the boundary between them (sharp bilinear).
        const int factor = std::min(pw / std::max(sw, 1), ph / std::max(sh, 1));
        if (factor >= 2) {
            if (const ScaleBuffer* big =
                    scale_buffer(next++, sw * factor, sh * factor)) {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, big->fbo);
                glBlitFramebuffer(sx, sy, sx + sw, sy + sh, 0, 0, big->w,
                                  big->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                src = big->fbo;
                sx = sy = 0;
                sw = big->w;
                sh = big->h;
            }
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        const GLint ox = m_present_x, oy = m_present_y;
        glBlitFramebuffer(sx, sy, sx + sw, sy + sh, ox, oy, ox + pw, oy + ph,
                          GL_COLOR_BUFFER_BIT,
                          sw == pw && sh == ph ? GL_NEAREST : GL_LINEAR);
    }

    // Single-sample intermediate planes for scaled presentation, by slot.
    struct ScaleBuffer {
        GLuint fbo = 0, rbo = 0;
        int w = 0, h = 0;
    };
    const ScaleBuffer* scale_buffer(size_t slot, int w, int h)
    {
        if (w <= 0 || h <= 0) return nullptr;
        if (m_scale_buffers.size() <= slot) m_scale_buffers.resize(slot + 1);
        ScaleBuffer& buffer = m_scale_buffers[slot];
        if (buffer.fbo && buffer.w == w && buffer.h == h) return &buffer;
        release_scale_buffer(buffer);
        glGenFramebuffers(1, &buffer.fbo);
        glGenRenderbuffers(1, &buffer.rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, buffer.rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, buffer.rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            release_scale_buffer(buffer);
            return nullptr;
        }
        buffer.w = w;
        buffer.h = h;
        return &buffer;
    }
    void release_scale_buffer(ScaleBuffer& buffer)
    {
        if (buffer.fbo) glDeleteFramebuffers(1, &buffer.fbo);
        if (buffer.rbo) glDeleteRenderbuffers(1, &buffer.rbo);
        buffer = ScaleBuffer{};
    }

    void touch(uint32_t base)
    {
        const auto found = m_targets.find(base);
        if (found != m_targets.end()) found->second.last_use = ++m_use_clock;
    }

    // Release the least-recently selected pooled base that no consumer still
    // requires. The requested base is not pooled yet; the active base
    // receives the list being bound and the presented base is blitted to the
    // drawable, so neither may go. Only the base's own color renderbuffer is
    // deleted: depth is shared by every target and outlives the release.
    // At most two of the sixteen pooled bases are protected, so a victim
    // always exists; the false return is the explicit, counted refusal path
    // and is never a throw.
    bool release_for(uint32_t requested)
    {
        auto victim = m_targets.end();
        for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
            if (it->first == requested || it->first == m_base ||
                it->first == m_present_base)
                continue;
            if (victim == m_targets.end() ||
                it->second.last_use < victim->second.last_use)
                victim = it;
        }
        if (victim == m_targets.end()) return false;
        glDeleteFramebuffers(1, &victim->second.fbo);
        glDeleteRenderbuffers(1, &victim->second.color);
        glDeleteRenderbuffers(1, &victim->second.ssb);
        m_targets.erase(victim);
        ++m_releases;
        report_capacity("release", requested);
        return true;
    }

    // A capacity event is never silent: the counters are permanent and the
    // first sixteen events are logged, matching the bridge's span-release
    // diagnostic, so a run that gave a target back is visible after the fact.
    void report_capacity(const char* kind, uint32_t requested)
    {
        static std::atomic<uint32_t> emitted{0};
        if (emitted.fetch_add(1) >= 16u) return;
        std::fprintf(stderr,
                     "[clt-target] capacity %s requested=%#x pool=%zu/%zu "
                     "active=%#x present=%#x releases=%llu refusals=%llu\n",
                     kind, requested, m_targets.size(), kMaxTargets, m_base,
                     m_present_base, (unsigned long long)m_releases,
                     (unsigned long long)m_refusals);
    }

    void bind_active()
    {
        if (m_base == 0) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }
        const auto found = m_targets.find(m_base);
        glBindFramebuffer(GL_FRAMEBUFFER,
                          found == m_targets.end() ? 0 : found->second.fbo);
    }

    std::map<uint32_t, Target> m_targets;
    uint32_t m_base = 0;
    uint32_t m_present_base = 0;
    bool m_crop_overscan = false;
    int m_present_x = 0;
    int m_present_y = 0;
    int m_present_w = 0;
    int m_present_h = 0;
    std::vector<ScaleBuffer> m_scale_buffers;
    GLuint m_depth = 0;
    GLuint m_snapshot_fbo = 0;
    // Single-sample resolve plane for multisample targets (see ensure_resolve).
    GLuint m_resolve_fbo = 0;
    GLuint m_resolve_rbo = 0;
    int m_resolve_w = 0, m_resolve_h = 0;
    int m_samples = 1;
    GLfloat m_depth_clear = 0.f;
    int m_w = 0, m_h = 0;
    GLfloat m_clear[4] = {0.05f, 0.07f, 0.10f, 1.f};
    uint64_t m_use_clock = 0;
    uint64_t m_releases = 0;
    uint64_t m_refusals = 0;
    void (*glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
    void (*glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
    void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
    GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
    void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
    void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void (*glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                              GLint, GLbitfield, GLenum) = nullptr;
    void (*glGenRenderbuffers)(GLsizei, GLuint*) = nullptr;
    void (*glDeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
    void (*glBindRenderbuffer)(GLenum, GLuint) = nullptr;
    void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
    void (*glRenderbufferStorageMultisample)(
        GLenum, GLsizei, GLenum, GLsizei, GLsizei) = nullptr;
    void (*glClearBufferfv)(GLenum, GLint, const GLfloat*) = nullptr;
    void (*glClearBufferiv)(GLenum, GLint, const GLint*) = nullptr;
    void (*glStencilFunc)(GLenum, GLint, GLuint) = nullptr;
    void (*glStencilOp)(GLenum, GLenum, GLenum) = nullptr;
    void (*glStencilMask)(GLuint) = nullptr;
    void (*glDrawBuffers)(GLsizei, const GLenum*) = nullptr;
    void (*glDisablei)(GLenum, GLuint) = nullptr;
    void (*glDepthRange)(GLdouble, GLdouble) = nullptr;
};

} // namespace m2host
