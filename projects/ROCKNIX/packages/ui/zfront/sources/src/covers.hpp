// Covers of the detailed view, decoded off the UI thread.
//
// Decoding a PNG/JPG cover and shrinking it takes >100 ms on the A35, which is a visible stall if done inside `draw`.
// `request` posts the work to the shared job thread (`cx.sv->jobs`): the worker only decodes and downscales (no `Assets`
// state is touched, see `Assets::decode_fit`); the finished image is inserted into `Assets` in the job's `done`, on the
// main thread, which also makes the loop redraw. Until then the cover area stays empty (like ES while it loads).
//
//  * Only the LAST request counts: scrolling past a game and settling on the next one must not queue a decode per game.
//    A job whose request was superseded before it started does nothing.
//  * One request per path+box at a time; a path that failed to decode is not tried again for 60 s (`failed`).
//  * The image lives in `Assets` under the usual `file_fit` key, so the byte budget and the eviction apply to it.
// Everything here runs on the main thread, except the worker lambda.
#pragma once
#include <cstdint>
#include <string>

#include "gfx.hpp"

namespace zf {

class Jobs;

namespace covers {

/// True once `path` (fitted into `max_w x max_h`) is in `a`; `id` is set.
bool ready(const Assets& a, const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id);
/// Starts the background decode unless the image is cached, already requested or known to be bad. Returns `ready`.
bool request(Assets& a, Jobs& jobs, const std::string& path, uint32_t max_w, uint32_t max_h);

/// Paths that could not be decoded (not retried for 60 s: then `failed` forgets them and the next `request` decodes again).
bool failed(const std::string& path);
void mark_failed(const std::string& path);
/// Queued decodes that have not started will not run (the game is about to start: they would hold their picture in RAM meanwhile).
/// Their `done` still runs after the game and the next `request` asks again.
void cancel_pending();
/// Forgets every failed path and gives the memory back (the game is about to start).
void clear_failed();
/// Tests: replaces the clock (seconds) of the expiry; null restores the real one.
void set_clock_for_test(double (*now)());

/// Requests started and not yet finished (tests).
size_t in_flight();

}  // namespace covers
}  // namespace zf
