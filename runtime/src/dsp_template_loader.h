// dsp_template_loader.h — on-disc M2 DSP template loader and DSPP simulator
// core behind the pure-synth StartInstrument boundary. See
// dsp_template_loader.c.
#ifndef M2_DSP_TEMPLATE_LOADER_H
#define M2_DSP_TEMPLATE_LOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSPP_MAX_NAME 32
#define DSPP_MAX_RESOURCES 32
#define DSPP_MAX_CODE_WORDS 128
#define DSPP_MAX_RELOCATIONS 32
#define DSPP_MAX_INITIALIZERS 8
// Retail sub_decode_adp4.dsp carries one DINI record with 89 values; the
// limit must cover it, and the parser rejects anything larger instead of
// writing past the array.
#define DSPP_MAX_INITIALIZER_VALUES 128
#define DSPP_DATA_MEMORY_WORDS 0x2E0u
#define DSPP_MAX_DYNAMIC_LINKS 8

typedef struct dspp_relocation {
    uint16_t resource_index;
    uint16_t part;
    uint8_t code_hunk;
    uint16_t code_offset;
} dspp_relocation;

typedef struct dspp_initializer {
    uint32_t flags;           // DINI_F_AT_ALLOC / DINI_F_AT_START
    uint32_t resource_index;
    uint32_t value_count;
    uint32_t values[DSPP_MAX_INITIALIZER_VALUES];
} dspp_initializer;

typedef struct dspp_resource {
    char name[DSPP_MAX_NAME];
    uint8_t type;      // DRSC_TYPE_*
    uint8_t subtype;
    uint8_t flags;     // DRSC_F_*
    uint32_t many;
    uint32_t allocated;     // template bind part (DRSC_F_BIND offset)
    uint32_t bind_index;
    int32_t default_value;
    uint32_t alloc_value;   // filled by the allocator, 0xFFFFFFFF = invalid
} dspp_resource;

typedef struct dspp_template {
    char name[DSPP_MAX_NAME];
    uint32_t function_id;
    uint32_t silicon_version;
    uint32_t format_version;
    uint32_t flags;
    uint32_t resource_count;
    dspp_resource resources[DSPP_MAX_RESOURCES];
    uint32_t code_word_count;
    uint16_t code_words[DSPP_MAX_CODE_WORDS];
    uint32_t relocation_count;
    dspp_relocation relocations[DSPP_MAX_RELOCATIONS];
    uint32_t initializer_count;
    dspp_initializer initializers[DSPP_MAX_INITIALIZERS];
    uint32_t dynamic_link_count;
    char dynamic_links[DSPP_MAX_DYNAMIC_LINKS][DSPP_MAX_NAME];
} dspp_template;

typedef struct dspp_sim dspp_sim;
typedef struct dspp_sim_space dspp_sim_space;
typedef struct dspp_delay_line dspp_delay_line;

// Parse a FORM 3INS/DSPP template from disc bytes. Returns 0 on success.
int dspp_load_template(const uint8_t *data, uint32_t size,
                       dspp_template *out);

// Build a relocated, executable instrument from a parsed template. The
// returned simulator owns a private copy of the resource allocation; the
// caller may free or reuse the template afterwards.
dspp_sim *dspp_sim_create(const dspp_template *t);
void dspp_sim_destroy(dspp_sim *s);

// M2 instruments share the M2's global data-memory, FIFO-channel, and
// trigger allocators. The standalone create path above retains private
// namespaces for focused loader tests; production audio binds instruments to
// one shared space so cross-instrument data connections and DSPI resource
// relocations receive the original global addresses.
dspp_sim_space *dspp_sim_space_create(void);
void dspp_sim_space_destroy(dspp_sim_space *space);
dspp_sim *dspp_sim_create_in_space(const dspp_template *t,
                                   dspp_sim_space *space);

// Reserve/release n words in the shared data image for a host-side
// mixer-output carrier (generated-mixer hop): the reserved range is real
// allocated DSPP data memory, so sim-side connections patch destination
// relocation sites with its address and read live values. Reserve fails
// (-1) when the allocator is exhausted; release zeroes freed words.
int dspp_sim_space_reserve_words(dspp_sim_space *space, uint32_t count,
                                 uint32_t *out_addr);
void dspp_sim_space_release_words(dspp_sim_space *space, uint32_t addr,
                                  uint32_t count);

// Ensure the shared space carries the on-disc nanokernel head instrument
// (M2 folio init loads System.m2/Audio/dsp/nanokernel.dsp before any
// client instrument). Its DRSC_F_EXPORT resources (gMixer, gScratch1/2/3,
// gReserved1-4, gBenchStart, rate-list jump labels) then resolve the
// importing instruments' DRSC_F_IMPORT resources by name. Returns 0 when
// the head is resident, -1 when it cannot load (no disc bridge / parse /
// allocation failure; latched until the space is recreated).
int dspp_sim_space_ensure_kernel(dspp_sim_space *space);

// M2 device/folio interrupt initialization for one DSPP addressable
// space (the M2 DSPP behavior:
// dspp_touch_bulldog.c dsphInitInterruptRegs +
// dspp_irq_bulldog.c InitDSPPInterrupt): clear the main partial bank and
// every second-level pending/enable bank, then leave DSPX_F_INT_DMANEXT
// enabled in the main enable bank (InitDSPPInterrupt's
// WriteHardware(DSPX_INTERRUPT_ENABLE, DSPX_F_INT_DMANEXT) after
// WriteHardware(DSPX_INTERRUPT_CLR, ~0)). The M2 has one DSPP, so this is
// space state; the folio FIRQ handler consumes it. Nothing is delivered
// here.
void dspp_sim_space_init_folio_interrupts(dspp_sim_space *space);

// Write one instruction word into an instrument's relocated code copy
// (M2 PatchCode16); the template image stays pristine.
void dspp_sim_poke_code(dspp_sim *s, uint32_t offset, uint16_t value);

// Resolve an exported resource name (e.g. "gMixer") to its allocated data
// address in the space's shared image; -1 when no live instrument exports
// the name (M2 dsppImportResource empty-list behavior).
int dspp_sim_space_export_address(dspp_sim_space *space, const char *name,
                                  uint32_t *out_addr);

// Revision of the space export table. A cached name resolution stays valid
// while this value is unchanged.
uint32_t dspp_sim_space_export_generation(const dspp_sim_space *space);

// Read/write one data word through the kernel (head) instrument's backing
// image. Reads return 0 and writes drop when no head is loaded or the
// address is outside the 0x2E0-word M2 data memory.
uint16_t dspp_sim_space_peek_kernel_data(dspp_sim_space *space,
                                         uint32_t addr);
void dspp_sim_space_poke_kernel_data(dspp_sim_space *space, uint32_t addr,
                                     uint16_t value);

// Run the head instrument's cyclic program for n audio frames (M2
// nanokernel execution: FrameCount advance, rate-list staging, DSPI
// output-commit observation). No-op without a resident head.
void dspp_sim_space_tick_kernel(dspp_sim_space *space, uint32_t frames);

// The relocated resource table (allocation + relocations applied).
const dspp_template *dspp_sim_resources(const dspp_sim *s);

// Run control: the guest's cyclic DSP execution only advances while the
// instrument is GWilling, and each audio frame runs until the code SLEEPs.
void dspp_sim_set_gwilling(dspp_sim *s, int willing);
void dspp_sim_tick(dspp_sim *s);
void dspp_sim_tick_n(dspp_sim *s, uint32_t frames);
void dspp_sim_reset_cycle(dspp_sim *s);

// Host-fed inputs (DSPI_INPUT0/1) and readback of the DAC output registers.
void dspp_sim_set_input(dspp_sim *s, uint32_t index, int16_t value);
int16_t dspp_sim_output(const dspp_sim *s, uint32_t index);

// Feed a FIFO channel from the host side (DMA direction into the DSPP).
void dspp_sim_fifo_push(dspp_sim *s, uint32_t channel, int16_t value);

// Audio Folio delay-line storage used by output-FIFO and input-FIFO sample
// attachments.  The Folio allocates delay-line memory in supervisor RAM,
// clears it on creation, and exposes it to the DSPP only through FDMA.  These
// helpers model that platform boundary; they do not create a guest sample or
// choose a graph route.  A line must be detached from every channel before
// it is destroyed.
dspp_delay_line *dspp_delay_line_create(uint32_t num_bytes,
                                        uint32_t num_channels, int if_loop);
void dspp_delay_line_clear(dspp_delay_line *line);
// Attach the authored AF_TAG_SUSTAIN*/AF_TAG_RELEASE* frame ranges to a
// delay-line Sample Item. Each range is either absent (-1, -1) or a valid
// half-open frame interval. This changes the physical attachment's startup
// and ReleaseInstrument schedule; it does not create a guest attachment or
// synthesize a successor/silence item. Returns 0 on success, -1 on malformed
// or out-of-range metadata.
int dspp_delay_line_set_sample_loops(dspp_delay_line *line,
                                     int32_t sustain_begin,
                                     int32_t sustain_end,
                                     int32_t release_begin,
                                     int32_t release_end);
// Set the AudioSample AF_SAMPF_CLEARONSTOP behavior. When enabled, the
// supervisor buffer is cleared once every bound attachment is stopped; this
// does not signal a Cue or synthesize a replacement attachment.
void dspp_delay_line_set_clear_on_stop(dspp_delay_line *line, int enabled);
void dspp_delay_line_destroy(dspp_delay_line *line);

// Start a delay-line attachment on a physical FIFO channel.  output != 0 is
// the M2 OUT_FIFO -> delay-line direction (delay_f1/delay_f2); output ==
// 0 is the delay-line -> IN_FIFO direction (sampler_raw/sampler attachments).
// start_frame is the AF_TAG_START_AT frame for the input direction and must
// be zero for output attachments.  The attachment starts with a cleared
// eight-word FIFO, an unbounded loop descriptor for circular lines, and the
// M2 4-word FDMA service threshold.  Returns 0 on success, -1 on an
// invalid channel/line/start frame or a conflicting live binding.
int dspp_sim_dma_attach_delay_line(dspp_sim *s, uint32_t channel,
                                   dspp_delay_line *line, int output,
                                   uint32_t start_frame);
// Restart an existing physical delay-line attachment from its original
// sample start. This is the full-start half of DSPPStartSampleAttachment:
// reset the channel FIFO/cursors and completion state while retaining the
// attachment's delay-line binding. Returns 0 on a valid owned binding.
int dspp_sim_dma_restart_delay_line(dspp_sim *s, uint32_t channel,
                                    dspp_delay_line *line, int output,
                                    uint32_t start_frame);
// Apply the DSPPReleaseSampleAttachment transition to a live physical sample
// binding. A sustain loop defers the transition until its current segment
// reaches the authored boundary; a release loop then runs indefinitely, and a
// missing release loop drains the authored tail. Samples without a sustain
// phase are already on their final schedule and return success as a no-op.
int dspp_sim_dma_release_delay_line(dspp_sim *s, uint32_t channel);
// Stop a bound delay-line attachment while retaining its binding. This is
// the platform-only equivalent of DSPPStopSampleAttachment: the line may be
// cleared by AF_SAMPF_CLEARONSTOP when all of its bindings are stopped.
// Returns 0 when a live owned binding was stopped, -1 otherwise.
int dspp_sim_dma_stop_delay_line(dspp_sim *s, uint32_t channel);
void dspp_sim_dma_detach_delay_line(dspp_sim *s, uint32_t channel);
// 1 when the channel carries a live Audio Folio delay-line attachment. This
// distinguishes physical delay completions from the separate numeric
// descriptor compatibility seam at the folio delivery boundary.
int dspp_sim_dma_delay_line_attached(const dspp_sim *s, uint32_t channel);
// Promote a stopped physical delay attachment to the next delay-line sample
// without resetting the DSPP FIFO. This is the bounded equivalent of
// ProcessAttachmentDMA -> DSPPStartSampleAttachment(next, FALSE): the prior
// completion has already been observed by the folio seam, the next line gets
// a fresh cursor, and any values already resident in the FIFO remain ordered.
// Returns 0 on a valid owned stopped binding, -1 otherwise.
int dspp_sim_dma_promote_delay_line(dspp_sim *s, uint32_t channel,
                                    dspp_delay_line *line, int output,
                                    uint32_t start_frame);
// Physical attachment state used by the folio teardown seam. These queries
// report only an existing delay-line binding; NULL/bad channels read 0.
int dspp_sim_dma_delay_line_stopped(const dspp_sim *s, uint32_t channel);
int dspp_sim_dma_delay_line_fifo_empty(const dspp_sim *s, uint32_t channel);

// Bounded host-buffer adapter for the M2 FDMA descriptor contract. A
// current/next count is in samples; input buffers are copied into the DSPP
// FIFO and DSPP-to-RAM buffers receive big-endian 16-bit samples. The
// adapter is platform state only and does not replace AudioAttachment/Cue
// delivery.
#define DSPP_SIM_DMA_TO_RAM 0x01u
#define DSPP_SIM_DMA_8BIT 0x02u
#define DSPP_SIM_DMA_SQXD 0x04u
int dspp_sim_dma_set_current(dspp_sim *s, uint32_t channel, void *buffer,
                             uint32_t sample_count, uint32_t flags);
int dspp_sim_dma_set_next(dspp_sim *s, uint32_t channel, void *buffer,
                          uint32_t sample_count);
void dspp_sim_dma_set_control(dspp_sim *s, uint32_t channel, int enabled,
                              int go_forever);
int dspp_sim_dma_step(dspp_sim *s);
uint32_t dspp_sim_dma_dmanext_interrupts(const dspp_sim *s);
void dspp_sim_dma_clear_dmanext_interrupts(dspp_sim *s, uint32_t mask);
// Armed (pending & enabled) DMA-interrupt dispatch (M2 behavior:
// the M2 DSPP behavior: dspp_simulator.c
// dspsGenMainInt + dspp_irq_bulldog.c dsphHandleDMANEXT +
// dspp_touch_bulldog.c dsphResetFIFO/dsphInitInterruptRegs/
// dsphEnableChannelInterrupt/dsphDisableChannelInterrupt +
// dspp_touch_dma.c DMA start path).
// Each DMA bank raises pending bits through the existing service paths (DMANext
// via dspsProcessNextDMA next-commit, Consumed via dspsReadFIFOtoDSPP
// drain-to-empty while ChannelComplete, UnderOver via DSPP-side FIFO underflow)
// or the M2 SET register; the folio FIRQ second level dispatches only
// pending & enabled (ValidInts = SET & ENABLE, dspp_irq_bulldog.c). This family
// exposes that dispatch as pure observation for a future folio delivery seam:
// enable sets enable-bank bits (DMANext through the same word the DMA CONTROL
// shadow and INT_DMANEXT_ENABLE raise; Consumed/UnderOver through their ENABLE
// registers), disable clears them (Consumed/UnderOver DISABLE registers;
// DMANext takes the shadow-control clear effect on the same word — the M2
// ships no INT_DMANEXT_DISABLE register), clear drops pending bits (the CLR
// registers; mirrors the M2 clear-stale-before-start discipline in
// dsphResetFIFO and the DMA start path), and the query returns armed bits per
// bank. No signal, Cue, or completion is raised here; delivery stays a folio
// seam. Bounds: banks selects among DSPP_DMA_BANK_* (unknown bits ignored);
// channel_mask is the 32-channel word (every bit addresses a channel, like the
// M2 MASK registers); NULL sims tolerate enable/disable/clear and read back
// zeros; nothing is ever synthesized from attached/enabled state, so unraised
// channels read 0 even with their enables on.
#define DSPP_DMA_BANK_DMANEXT 0x01u
#define DSPP_DMA_BANK_CONSUMED 0x02u
#define DSPP_DMA_BANK_UNDEROVER 0x04u
#define DSPP_DMA_BANK_ALL \
    (DSPP_DMA_BANK_DMANEXT | DSPP_DMA_BANK_CONSUMED | DSPP_DMA_BANK_UNDEROVER)
typedef struct dspp_dma_armed {
    uint32_t dmanext;   // pending & enabled channel bits
    uint32_t consumed;
    uint32_t underover;
} dspp_dma_armed;
void dspp_sim_dma_enable_interrupts(dspp_sim *s, uint32_t banks,
                                    uint32_t channel_mask);
void dspp_sim_dma_disable_interrupts(dspp_sim *s, uint32_t banks,
                                     uint32_t channel_mask);
void dspp_sim_dma_clear_interrupts(dspp_sim *s, uint32_t banks,
                                   uint32_t channel_mask);
// Armed dispatch: pending & enabled per bank (M2 ValidInts). NULL reads
// zeros; enable-off masks everything; unraised channels read 0.
dspp_dma_armed dspp_sim_dma_armed_interrupts(const dspp_sim *s);

// ---- DSPP FIRQ first-level dispatch (M2 dspp_irq_bulldog.c) -------------
// The DSPP main-interrupt word is compositional (M2 dspp_simulator.c
// dspsGenMainInt): the stored partial bank (soft ints, timer) OR the armed
// DMANext/Consumed second-level bits. Writes to DSPX_INTERRUPT_SET/CLR touch
// only the non-DMA partial bits; DSPX_INTERRUPT_ENABLE/DISABLE are
// write-set/write-clear banks. These constants expose that register layout to
// the folio seam.
#define DSPP_MAIN_INT_DMANEXT 0x00000001u
#define DSPP_MAIN_INT_CONSUMED 0x00000002u
#define DSPP_MAIN_INT_UNDEROVER 0x00000008u
#define DSPP_MAIN_INT_TIMER 0x00000100u
#define DSPP_MAIN_INT_SOFT_SHIFT 16u
#define DSPP_MAIN_INT_SOFT_MASK 0xFFFF0000u
// Composed main-interrupt word (dspsGenMainInt) and the main enable bank.
uint32_t dspp_sim_dma_main_interrupt(const dspp_sim *s);
uint32_t dspp_sim_dma_main_interrupt_enable(const dspp_sim *s);
// M2 dspsCheckTimerInterrupt/dspsAdvanceAudioTimer raise site: set the
// timer bit in the partial bank. Returns 1 when newly raised, 0 when already
// pending or NULL.
int dspp_sim_dma_raise_timer_interrupt(dspp_sim *s);
// M2 dsphEnable/DisableChannelInterrupt: enable/disable one channel's
// second-level DMANext interrupt through the DMA-control shadow word
// (DSPX_DMA_CONTROL_CURRENT). The DMANext enable has no standalone DISABLE
// register; this is the M2 path. Enable state reads through the shared
// per-channel enable bank.
void dspp_sim_dma_set_channel_interrupt(dspp_sim *s, uint32_t channel,
                                        int enabled);
int dspp_sim_dma_channel_interrupt_enabled(const dspp_sim *s,
                                           uint32_t channel);
// M2 dsphFIRQHandler pass over one live sim's shared banks:
// validInts = SET & ENABLE; service in M2 precedence order
// DMANext -> timer -> soft (trigger) -> unexpected (ignored). One pass over
// the currently valid bits, exactly like the M2 handler's early-out
// chain.
//  - dmanext: per-channel second-level pending serviced (M2
//    dsphHandleDMANEXT ValidInts = DSPX_INT_DMANEXT_SET & ENABLE, FindMSB
//    order). Each serviced channel's pending bit is cleared after service and
//    its DMANext enable is cleared (M2 all-done disable: the host DMA
//    adapter commits the queued next descriptor when the event is raised, so
//    both of the M2's folio countdowns are already zero here; a later
//    segment re-arms through dspp_sim_dma_set_channel_interrupt).
//  - timer: 1 when the timer bit was valid and cleared (dsphHandleTimer).
//  - soft: received soft bits after the M2 dsphHandleSoftInt disable step
//    (DSPX_INTERRUPT_DISABLE); the daemon half (dsphGetReceivedTriggers) reads
//    SET and clears exactly the armed-received bits.
// Raises no signal, Cue, or completion: the folio daemon owns delivery. NULL
// sims return a zero dispatch.
typedef struct dspp_firq_dispatch {
    uint32_t dmanext; // serviced second-level channel bits (daemon mask)
    int timer;        // 1 when the timer interrupt was cleared
    uint32_t soft;    // received soft bits, already disabled
} dspp_firq_dispatch;
dspp_firq_dispatch dspp_sim_dma_firq_dispatch(dspp_sim *s);

// Raw DSPX DMA-stack/control and interrupt-register boundary used by the
// M2 audio folio. Addresses are byte addresses (for example 0x65000 +
// channel*0x10 for the descriptor stack and 0x65200 + channel*0x10 for
// control). Shadow address/count writes become current/next state only on the
// M2 control commit bit. Numeric descriptors do not create a host pointer
// by themselves.
uint32_t dspp_sim_dma_register_read(const dspp_sim *s, uint32_t addr);
void dspp_sim_dma_register_write(dspp_sim *s, uint32_t addr,
                                 uint32_t value);
// Numeric (non-sim) DSPP DMA descriptor attach (M2 behavior:
// the M2 DSPP behavior: dspp_simulator.c
// dspsAccessDMA_Stack + audio_attachments.c +
// dspp_instr.c DSPPAttachSample). Parses/validates a raw numeric
// address/count descriptor and binds it to the addressed DMA-channel resource
// with stated bounds. Out-of-range/mistyped descriptors are rejected with a
// M2-mapped error; nothing is fabricated (no host pointer is created and
// the DMA/FIFO/interrupt/completion service never reads this record — it
// still requires a host buffer via dspp_sim_dma_set_current/next).
// Bounds: channel 0..31 (DSPI_NUM_DMA_CHANNELS; pass-2 32 in dsp/dspp_addresses.j;
// paranoid channel check in dspp_instr.c); address 25-bit 0..0x1FFFFFF
// (DSPX_DMA_ADDRESS_MASK, audio_attachments.c); count in samples, 0 = empty
// descriptor (mirrors the set_current count-0 rule; sample-length cross-checks
// live at the folio AF_TAG_FRAMES/NUMBYTES layer); flags subset of
// TO_RAM|8BIT|SQXD with SQXD requiring 8BIT (DRSC_INFIFO_SUBTYPE_SQS2 is an
// 8-bit compressed input subtype; the service decodes SQXD only when width8).
// FIFO IN/OUT mistype (ordinary sample to OUT_FIFO = SECURITY, non-FIFO hook =
// BAD_PORT_TYPE, unknown hook = BAD_NAME) and busy live at the folio attach
// seam (see proposed runtime hunk); this layer validates numeric shape only.
// Error mapping: BAD_ITEM = folio AF_ERR_BADITEM, OUT_OF_RANGE = AF_ERR_OUTOFRANGE,
// BAD_TYPE = AF_ERR_BADTAGVAL (illegal/mistyped flags), NULL_ADDRESS =
// AF_ERR_NULLADDRESS (runtime maps it to M2_AUDIO_ERR_BAD_PTR per
// ValidateAudioItemData/afi_IsRamAddr ground; busy maps to M2_AUDIO_ERR_NO_MEMORY,
// pure-synth/no-upload stays UNIMPLEMENTED, 0x4E/0x4F retained uninterpreted).
#define DSPP_NUMERIC_ADDR_MASK 0x01FFFFFFu
#define DSPP_NUMERIC_LEGAL_FLAGS \
    (DSPP_SIM_DMA_TO_RAM | DSPP_SIM_DMA_8BIT | DSPP_SIM_DMA_SQXD)
#define DSPP_NUMERIC_OK 0
#define DSPP_NUMERIC_ERR_BAD_ITEM -1
#define DSPP_NUMERIC_ERR_OUT_OF_RANGE -2
#define DSPP_NUMERIC_ERR_BAD_TYPE -3
#define DSPP_NUMERIC_ERR_NULL_ADDRESS -4
int dspp_sim_dma_attach_numeric(dspp_sim *s, uint32_t channel,
                                uint32_t address, uint32_t count,
                                uint32_t flags);
// 1 when channel holds a validated numeric descriptor, else 0 (bad channel or
// NULL sim reads 0; never fabricates).
int dspp_sim_dma_numeric_attached(const dspp_sim *s, uint32_t channel);
// Validated descriptor readback. Returns DSPP_NUMERIC_OK with outputs stored,
// or the attach-equivalent error (BAD_ITEM/OUT_OF_RANGE for addressing, and
// BAD_TYPE is never returned here — a stored record is already validated).
// Unattached channels report zeros with DSPP_NUMERIC_OK.
int dspp_sim_dma_numeric_descriptor(const dspp_sim *s, uint32_t channel,
                                    uint32_t *address_out, uint32_t *count_out,
                                    uint32_t *flags_out);
// Raw-DSPP consumed generation (smallest faithful completion-qualified step).
// M2 behavior: dspsReadFIFOtoDSPP drains one FIFO word per
// successful DSPP-side read and raises the per-channel Consumed pending bit
// only when the drain empties the FIFO while the M2 ChannelComplete bit
// is already set; dspsReadFIFOtoDMA has no Consumed side effect and
// CURRENT/NEXT peeks never consume. This counter records the same drain
// site (one successful dspp_fifo_read_dspi word on a numeric-attached
// channel = one consumed word) as a 32-bit saturating generation, without
// raising any interrupt, signal, or completion: delivery stays a separate
// folio seam. Live current_*/host buffers, FIFO words/cursors, and interrupt
// banks are untouched by the count itself. Successful attach resets the
// channel generation to 0 (failed attaches leave it); reset/re-attach
// replaces. Query reads 0 for NULL sims, out-of-range channels, and
// unattached channels (never fabricates). The counter saturates at
// 0xFFFFFFFFu and never wraps.
uint32_t dspp_sim_dma_numeric_consumed(const dspp_sim *s, uint32_t channel);
// Completion-qualified event record on a numeric-attached channel or a finite
// physical delay-line attachment (M2 behavior: ProcessAttachmentDMA
// decrements SegmentCount per serviced segment and calls
// SignalMonitoringCue exactly when it reaches 0). Numeric completion occurs
// when the consumed generation above reaches numeric_count; a physical delay
// completion occurs when its finite line reaches its final word. In either
// case one event is recorded in a bounded FIFO poll queue — one queue per DSPP
// addressable space (shared across sims like all DMA state; standalone sims
// carry a private queue) — without raising any guest signal, Cue, interrupt,
// or bank bit. Delivery stays at the existing MonitorAttachment pump seam
// (recomp_runtime.c gk_pump_attachment_completions; this layer never signals).
// Once per bind: the done latch suppresses re-queueing after polls and
// over-drains; only a successful re-attach releases it (and dequeues any
// still-queued event for the channel). Explicit stop of a physical line is
// not a completion edge. Edges: numeric count == 0 and looping delay lines
// never complete; unattached channels never complete; failed attaches leave
// prior state undisturbed; nothing is fabricated.
// Bound: DSPP_COMPLETION_QUEUE_CAP entries, deduped per channel so the bound
// holds by construction; a defensive overflow drops the oldest entry
// (documented, event discarded via the retained done latch) and preserves
// the newest completion.
#define DSPP_COMPLETION_QUEUE_CAP 32u
// 1 when channel has a queued numeric or finite physical completion event,
// else 0. Bad channel, NULL sim, and unattached channels read 0 (never
// fabricates).
int dspp_sim_dma_completion_pending(const dspp_sim *s, uint32_t channel);
// Queued depth, 0..DSPP_COMPLETION_QUEUE_CAP. NULL sims read 0.
uint32_t dspp_sim_dma_completion_queued(const dspp_sim *s);
// Dequeue the oldest queued completion into *channel_out (NULL tolerated:
// still dequeues, discards the id). Returns 1 dequeued, 0 when empty (out
// zeroed, never fabricates). NULL sims read empty. The dequeued channel's
// pending flag clears; its done latch is retained (no re-queue for the same
// bind — re-attach starts the next lifecycle).
int dspp_sim_dma_poll_completion(dspp_sim *s, uint32_t *channel_out);

// dsppConnectWithMove equivalent (M2 dspp_instr.c): record a
// per-frame MOVE copy data[dst_addr] = data[src_addr] on the destination sim
// for a BIND-to-RBASE destination. The M2 writes a
// DSPN_OPCODE_MOVEADDR/DSPN_OPERAND_ADDR word pair into the shared MOVE code
// block and repoints the header CallConnectionMoves JSR; the host has no
// shared code space, so the copy is recorded here and applied at each tick
// entry instead. Addresses are masked to the 10-bit MOVE-direct decode.
// Re-connecting the same destination word replaces the record; records past
// the bound drop without fabrication. Standalone private sims read the
// destination's local word; runtime sims in a dspp_sim_space alias the
// M2-wide data image.
void dspp_sim_connect_move(dspp_sim *dst, uint32_t dst_addr,
                           uint32_t src_addr);
// dsppConnectInstrumentParts equivalent: BIND-to-RBASE INPUT/KNOB names take
// the MOVE branch above (no destination code word is patched, matching the
// M2's if/else); other INPUT/KNOB names patch the (resource, part)
// relocation sites with the full source address operand
// (DSPN_OPERAND_ADDR | src_addr), walking the template's relative relocation
// links. Only relocation sites whose part equals dst_part are patched,
// matching the M2's (DstIndex, DstPart) match. Call after
// dspp_sim_create and before the first tick.
void dspp_sim_connect(dspp_sim *s, const char *input_name, uint32_t dst_part,
                      uint16_t src_addr_operand);
// dsppDisconnectInstrumentParts equivalent: MOVE-connected names drop the
// recorded copy (destination code and data memory undisturbed, matching the
// M2's early return after dsppDisconnectWithMove); other INPUT/KNOB
// names restore the relocation default (dsppGetResourceRelocationValue) at
// their sites. Other connections and data memory are undisturbed.
void dspp_sim_disconnect(dspp_sim *s, const char *input_name,
                         uint32_t dst_part);

// Direct data-memory access for DINI initializers and bounded diagnostics.
void dspp_sim_poke_data(dspp_sim *s, uint32_t addr, uint16_t value);
uint16_t dspp_sim_peek_data(const dspp_sim *s, uint32_t addr);

// Direct DSPI register access for bounded hardware-contract tests. Addresses
// use the M2's 10-bit DSPI data/register map (for example 0x302 is the
// FIFO oscillator frequency/last-count register and 0x303 is the shared
// phase register). Reads of FIFO data retain the M2's consuming behavior.
uint16_t dspp_sim_dspi_read(dspp_sim *s, uint32_t addr);
void dspp_sim_dspi_write(dspp_sim *s, uint32_t addr, uint16_t value);

#ifdef __cplusplus
}
#endif

#endif /* M2_DSP_TEMPLATE_LOADER_H */
