# The changes Crux2's RADV carries

`ps5/tools/build-radv-crux2.sh` applies these, in order, to PS5 Mesa at the revision PS5_Vulkan
pins, and builds the archive Crux2 links (`.deps/native/radv-crux2` in PS5_Vulkan).

| Patch | From | What it does |
| --- | --- | --- |
| `0001-diagnostic-breadcrumbs` | Crux2 | A few log lines at device start-up, so a start that stops inside the driver says where |
| `0002-threaded-recording-queues-push-descriptors` | Crux2 | Threaded recording queues `vkCmdPushDescriptorSet` instead of waiting for its thread (PCSX2's renderer pushes descriptors for most draws) |
| `0003-videoout-120hz-settle` | Crux2 | A television switching to 119.88 Hz gets up to 1.5 s to settle before 59.94 Hz is restored |
| `0004-ps5-timed-waits-on-the-condvar-clock` | Crux2 | The VideoOut WSI's and the winsys's timed waits get deadlines on the clock their condition variable keeps; with monotonic ones they returned at once and spun |
| `0007-radv-ps5-a-destroyed-buffer-s-memory-waits-for-the-G` | [PS5CEMU-HAR](https://github.com/premohq/PS5CEMU-HAR) `patches/mesa` (GPL-3.0), as [PS5LM](https://github.com/cobanov/PS5LM) carries it | A destroyed buffer's memory is freed only once the GPU has run the work submitted before; freed at once, the GPU could reach unmapped memory and the console hangs |
| `0009-radv-ps5-the-GPU-waits-spin-and-sleep-chosen-at-run-` | PS5CEMU-HAR, as PS5LM carries it | `RADV_PS5_WAIT_SPIN_US` and `RADV_PS5_WAIT_SLEEP_US` choose how long a wait for the GPU checks in a loop and then sleeps (1500 and 1000 us by default, as before); a fence status query no longer reads the clock. Crux2 sets them from Settings > Interface > PlayStation 5 |
| `0010-radv-threaded-recording-the-worker-reads-the-clock-once-in-64-pauses` | Crux2 | The recording thread's wait for work reads the clock once in 64 pauses instead of at every one (its clock reads kept a memory line bouncing between cores, slowing the other threads' reads) |
| `0011-wsi-videoout-a-title-sets-how-far-it-may-run-ahead-of-the-screen` | Crux2 | `wsi_videoout_set_queue_depth`: an acquire waits while that many images are presented and not yet shown (0, the default: only for a free image, as before). Crux2 keeps two while a game keeps up and allows three, with four images, while its frames come late, so a frame waiting for its vblank does not hold the next one back |
| `0012-mesa-26.2.5` | [Mesa](https://gitlab.freedesktop.org/mesa/mesa) 26.2.1-26.2.5 (MIT), merged by Crux2 | Mesa's stable releases since 26.2.0, merged into the PS5 RADV (tags `mesa-26.2.4` and `mesa-26.2.5`): binning on GFX10, sRGB clears by compute, depth/stencil resolves, immutable samplers' lifetime, ACO's constant and register-allocation fixes, the gang semaphore's own buffer, HTILE fills and more (`docs/relnotes/26.2.{1..5}.rst` in Mesa). Where upstream and the PS5 changes met, both were kept: the geometry shaders run as compute go first in every draw, then upstream's draw (with its new draw count) |

0007 and 0009 keep their authors' commit messages and authorship. Mesa's licences apply to all of
them (the PS5 winsys is MIT, like the code around it; PS5CEMU-HAR's patches are published under
GPL-3.0).
