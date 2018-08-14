/* === S Y N F I G ========================================================= */
/*!	\file renderer_canvas.cpp
**	\brief Template File
**
**	$Id$
**
**	\legal
**	Copyright (c) 2002-2005 Robert B. Quattlebaum Jr., Adrian Bentley
**	Copyright (c) 2007, 2008 Chris Moore
**  Copyright (c) 2011 Nikita Kitaev
**  ......... ... 2018 Ivan Mahonin
**
**	This package is free software; you can redistribute it and/or
**	modify it under the terms of the GNU General Public License as
**	published by the Free Software Foundation; either version 2 of
**	the License, or (at your option) any later version.
**
**	This package is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
**	General Public License for more details.
**	\endlegal
*/
/* ========================================================================= */

/* === H E A D E R S ======================================================= */

#ifdef USING_PCH
#	include "pch.h"
#else
#ifdef HAVE_CONFIG_H
#	include <config.h>
#endif

#include <ctime>
#include <cstring>
#include <valarray>

#include <glib.h>
#include <gdkmm/general.h>

#include <ETL/misc>

#include <synfig/general.h>
#include <synfig/canvas.h>
#include <synfig/context.h>
#include <synfig/threadpool.h>
#include <synfig/debug/measure.h>
#include <synfig/rendering/renderer.h>
#include <synfig/rendering/common/task/tasktransformation.h>

#include <gui/localization.h>
#include <gui/app.h>

#include "renderer_canvas.h"

#endif

/* === U S I N G =========================================================== */

using namespace std;
using namespace etl;
using namespace synfig;
using namespace studio;

/* === M A C R O S ========================================================= */

#ifndef NDEBUG
//#define DEBUG_TILES
#endif

/* === G L O B A L S ======================================================= */

/* === P R O C E D U R E S ================================================= */

static int
int_floor(int x, int base)
	{ int m = x % base; return m < 0 ? x - base - m : m > 0 ? x - m : x; }

static int
int_ceil(int x, int base)
	{ int m = x % base; return m > 0 ? x + base - m : m < 0 ? x - m : x; }

static long long
image_rect_size(const RectInt &rect)
	{ return 4ll*rect.get_width()*rect.get_height(); }

/* === M E T H O D S ======================================================= */

Renderer_Canvas::Renderer_Canvas():
	max_tiles_size_soft(512*1024*1024),
	max_tiles_size_hard(max_tiles_size_soft + 128*1024*1024),
	weight_future  (   1.0), // high priority
	weight_past    (   2.0), // low priority
	weight_zoom_in (1024.0), // very very low priority
	weight_zoom_out(1024.0),
	tiles_size(),
	pixel_format(),
	draw_queued()
{
	// check endianess
    union { int i; char c[4]; } checker = {0x01020304};
    bool big_endian = checker.c[0] == 1;

    pixel_format = big_endian
		         ? (PF_A_START | PF_RGB | PF_A_PREMULT)
		         : (PF_BGR | PF_A | PF_A_PREMULT);

	alpha_src_surface = Cairo::ImageSurface::create(
		Cairo::FORMAT_ARGB32, 1, 1);
	alpha_dst_surface = Cairo::ImageSurface::create(
		Cairo::FORMAT_ARGB32, 1, 1);

	//! fill alpha_src_surface with white color
	//! alpha premulted - so all four channels have the same value
	unsigned char *data = alpha_src_surface->get_data();
	data[0] = data[1] = data[2] = data[3] = 255;
	alpha_src_surface->mark_dirty();
	alpha_src_surface->flush();

	alpha_context = Cairo::Context::create(alpha_dst_surface);
}

Renderer_Canvas::~Renderer_Canvas()
	{ clear_render(); }

void
Renderer_Canvas::on_tile_finished_callback(bool success, Renderer_Canvas *obj, Tile::Handle tile)
{
	// This method may be called from the other threads
	// Handle will protect 'tile' from deletion before this call
	// This callback will called only once for each tile
	// And will called before deletion of 'obj', by calling cancel_render() from destructor
	obj->on_tile_finished(success, tile);
}

void
Renderer_Canvas::on_post_tile_finished_callback(etl::handle<Renderer_Canvas> obj, Tile::Handle tile) {
	// this function should be called in main thread
	// Handle will protect 'obj' from deletion before this call
	// zero 'work_area' means that 'work_area' is destructed and here is a last Handle of 'obj'
	if (obj->get_work_area())
		obj->on_post_tile_finished(tile);
}

Cairo::RefPtr<Cairo::ImageSurface>
Renderer_Canvas::convert(
	const synfig::rendering::SurfaceResource::Handle &surface,
	int width, int height ) const
{
	// this method may be called from the other threads
	assert(width > 0 && height > 0);

	Cairo::RefPtr<Cairo::ImageSurface> cairo_surface =
		Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, width, height);

	bool success = false;

	rendering::SurfaceResource::LockReadBase surface_lock(surface);
	if (surface_lock.get_resource() && surface_lock.get_resource()->is_blank()) {
		success = true;
	} else
	if (surface_lock.convert(rendering::Surface::Token::Handle(), false, true)) {
		debug::Measure measure("Renderer_Canvas::convert");

		const rendering::Surface &s = *surface_lock.get_surface();
		int w = s.get_width();
		int h = s.get_height();
		if (w == width && h == height) {
			const Color *pixels = s.get_pixels_pointer();
			std::vector<Color> pixels_copy;
			if (!pixels) {
				pixels_copy.resize(w*h);
				if (s.get_pixels(&pixels_copy.front()))
					pixels = &pixels_copy.front();
			}
			if (pixels) {
				// do conversion
				cairo_surface->flush();
				const Color *src = pixels;
				unsigned char *begin = cairo_surface->get_data();
				int stride = cairo_surface->get_stride();
				unsigned char *end = begin + stride*cairo_surface->get_height();
				for(unsigned char *row = begin; row < end; row += stride)
					for(unsigned char *pixel = row, *pixel_end = row + stride; pixel < pixel_end; )
						pixel = Color2PixelFormat(*src++, pixel_format, pixel, App::gamma);
				cairo_surface->mark_dirty();
				cairo_surface->flush();

				success = true;
			} else synfig::error("Renderer_Canvas::convert: cannot access surface pixels - that really strange");
		} else synfig::error("Renderer_Canvas::convert: surface with wrong size");
	} else synfig::error("Renderer_Canvas::convert: surface not exists");


	#ifdef DEBUG_TILES
	const bool debug_tiles = true;
	#else
	const bool debug_tiles = false;
	#endif

	// paint tile
	if (debug_tiles || !success) {
		Cairo::RefPtr<Cairo::Context> context = Cairo::Context::create(cairo_surface);

		if (!success) {
			// draw cross
			context->move_to(0.0, 0.0);
			context->line_to((double)width, (double)height);
			context->move_to((double)width, 0.0);
			context->line_to(0.0, (double)height);
			context->stroke();
		}

		// draw border
		context->rectangle(0, 0, width, height);
		context->stroke();
		std::valarray<double> dash(2); dash[0] = 2.0; dash[1] = 2.0;
		context->set_dash(dash, 0.0);
		context->rectangle(4, 4, width-8, height-8);
		context->stroke();

		cairo_surface->flush();
	}
	return cairo_surface;
}

void
Renderer_Canvas::on_tile_finished(bool success, const Tile::Handle &tile)
{
	// this method must be called from on_tile_finished_callback()
	// this method may be called from other threads

	// 'tiles', 'onion_frames', 'refresh_id', and 'tiles_size' are controlled by mutex

	Glib::Threads::Mutex::Lock lock(mutex);
	if (!tile->event && !tile->surface && !tile->cairo_surface)
		return; // tile is already removed

	tile->event.reset();
	if (success && tile->surface)
		tile->cairo_surface = convert(tile->surface, tile->rect.get_width(), tile->rect.get_height());
	tile->surface.reset();

	// don't create handle if ref-count is zero
	// it means that object was nether had a handles and will removed with handle
	// or object is already in destruction phase
	if (shared_object::count())
		Glib::signal_idle().connect_once(
			sigc::bind(sigc::ptr_fun(&on_post_tile_finished_callback), etl::handle<Renderer_Canvas>(this), tile), Glib::PRIORITY_HIGH);
}

void
Renderer_Canvas::on_post_tile_finished(const Tile::Handle &tile)
{
	// this method must be called from on_post_tile_finished_callback()
	// check if rendering is finished
	bool tile_visible = false;
	bool all_finished = true;
	{
		Glib::Threads::Mutex::Lock lock(mutex);
		if (visible_frames.count(tile->frame_id))
			tile_visible = true;
		for(TileMap::const_iterator i = tiles.begin(); all_finished && i != tiles.end(); ++i)
			for(TileList::const_iterator j = i->second.begin(); all_finished && j != i->second.end(); ++j)
				if (*j && (*j)->event)
					all_finished = false;
	}

	if (all_finished) enqueue_render();
	if (!draw_queued /*&& tile_visible*/)
		{ get_work_area()->queue_draw(); draw_queued = true; }
}

void
Renderer_Canvas::insert_tile(TileList &list, const Tile::Handle &tile)
{
	// this method may be called from other threads
	// mutex must be already locked
	list.push_back(tile);
	tiles_size += image_rect_size(tile->rect);
}

void
Renderer_Canvas::erase_tile(TileList &list, TileList::iterator i, rendering::Task::List &events)
{
	// this method may be called from other threads
	// mutex must be already locked
	if ((*i)->event) events.push_back((*i)->event);
	tiles_size -= image_rect_size((*i)->rect);
	(*i)->event.reset();
	(*i)->surface.reset();
	(*i)->cairo_surface.clear();
	list.erase(i);
}

void
Renderer_Canvas::remove_extra_tiles(synfig::rendering::Task::List &events)
{
	// mutex must be already locked

	typedef std::multimap<Real, TileMap::iterator> WeightMap;
	WeightMap sorted_frames;

	Real current_zoom = sqrt((Real)(current_frame.width * current_frame.height));

	// calc weight
	for(TileMap::iterator i = tiles.begin(); i != tiles.end(); ++i) {
		if (!visible_frames.count(i->first) && tiles_size > max_tiles_size_hard) {
			Real weight = 0.0;
			if (frame_duration) {
				Time dt = i->first.time - current_frame.time;
				Real df = ((double)dt)/(double)frame_duration;
				weight += df*(df > 0.0 ? weight_future : weight_past);
			}
			if (current_zoom) {
				Real zoom = sqrt((Real)(i->first.width * i->first.height));
				Real zoom_step = log(zoom/current_zoom);
				weight += zoom_step*(zoom_step > 0.0 ? weight_zoom_in : weight_zoom_out);
			}
			sorted_frames.insert( WeightMap::value_type(weight, i) );
		}
	}

	// remove some extra tiles to free the memory
	for(WeightMap::reverse_iterator ri = sorted_frames.rbegin(); ri != sorted_frames.rend() && tiles_size > max_tiles_size_hard; ++ri)
		for(TileList::iterator j = ri->second->second.begin(); j != ri->second->second.end() && tiles_size > max_tiles_size_hard; )
			erase_tile(ri->second->second, j++, events);

	// remove empty entries from tiles map
	for(TileMap::iterator i = tiles.begin(); i != tiles.end(); )
		if (i->second.empty()) tiles.erase(i++); else ++i;
}

void
Renderer_Canvas::build_onion_frames()
{
	// mutex must be already locked

	Canvas::Handle canvas    = get_work_area()->get_canvas();
	int            w         = get_work_area()->get_w();
	int            h         = get_work_area()->get_h();
	int            past      = std::max(0, get_work_area()->get_onion_skins()[0]);
	int            future    = std::max(0, get_work_area()->get_onion_skins()[1]);
	Time           base_time = canvas->get_time();
	RendDesc       rend_desc = canvas->rend_desc();
	float          fps       = rend_desc.get_frame_rate();

	current_frame = FrameId(base_time, w, h);
	frame_duration = Time(approximate_greater_lp(fps, 0.f) ? 1.0/(double)fps : 0.0);

	// set onion_frames
	onion_frames.clear();
	if ( get_work_area()->get_onion_skin()
	  && frame_duration
	  && (past > 0 || future > 0) )
	{
		const Color color_past  (1.f, 0.f, 0.f, 0.2f);
		const Color color_future(0.f, 1.f, 0.f, 0.2f);
		const ColorReal base_alpha = 1.f;
		const ColorReal current_alpha = 0.5f;
		// make onion levels
		for(int i = past; i > 0; --i) {
			Time time = base_time - frame_duration*i;
			ColorReal alpha = base_alpha + (ColorReal)(past - i + 1)/(ColorReal)(past + 1);
			if (time >= rend_desc.get_time_start() && time <= rend_desc.get_time_end())
				onion_frames.push_back(FrameDesc(time, w, h, alpha));
		}
		for(int i = future; i > 0; --i) {
			Time time = base_time + frame_duration*i;
			ColorReal alpha = base_alpha + (ColorReal)(future - i + 1)/(ColorReal)(future + 1);
			if (time >= rend_desc.get_time_start() && time <= rend_desc.get_time_end())
				onion_frames.push_back(FrameDesc(time, w, h, alpha));
		}
		onion_frames.push_back(FrameDesc(current_frame, base_alpha + 1.f + current_alpha));

		// normalize
		ColorReal summary = 0.f;
		for(FrameList::const_iterator i = onion_frames.begin(); i != onion_frames.end(); ++i)
			summary += i->alpha;
		ColorReal k = approximate_greater(summary, ColorReal(1.f)) ? 1.f/summary : 1.f;
		for(FrameList::iterator i = onion_frames.begin(); i != onion_frames.end(); ++i)
			i->alpha *= k;
	} else {
		onion_frames.push_back(FrameDesc(current_frame, 1.f));
	}

	// set visible_frames
	visible_frames.clear();
	for(FrameList::const_iterator i = onion_frames.begin(); i != onion_frames.end(); ++i)
		visible_frames.insert(i->id);
}

bool
Renderer_Canvas::enqueue_render_frame(const rendering::Renderer::Handle &renderer, const FrameId &id)
{
	// mutex must be already locked

	const int tile_grid_step = 64;

	Canvas::Handle canvas      = get_work_area()->get_canvas();
	RendDesc       rend_desc   = canvas->rend_desc();
	RectInt        window_rect = get_work_area()->get_window_rect();
	int            w           = get_work_area()->get_w();
	int            h           = get_work_area()->get_h();
	RectInt        full_rect   = RectInt(0, 0, w, h);

	rend_desc.clear_flags();
	rend_desc.set_wh(w, h);
	ContextParams context_params(rend_desc.get_render_excluded_contexts());
	TileList &frame_tiles = tiles[id];

	// create transformation matrix to flip result if needed
	bool transform = false;
	Matrix matrix;
	Vector p0 = rend_desc.get_tl();
	Vector p1 = rend_desc.get_br();
	if (p0[0] > p1[0] || p0[1] > p1[1]) {
		if (p0[0] > p1[0]) { matrix.m00 = -1.0; matrix.m20 = p0[0] + p1[0]; std::swap(p0[0], p1[0]); }
		if (p0[1] > p1[1]) { matrix.m11 = -1.0; matrix.m21 = p0[1] + p1[1]; std::swap(p0[1], p1[1]); }
		rend_desc.set_tl_br(p0, p1);
		transform = true;
	}

	// find not actual regions
	std::vector<synfig::RectInt> rects;
	rects.reserve(20);
	rects.push_back(window_rect);
	for(TileList::const_iterator j = frame_tiles.begin(); j != frame_tiles.end(); ++j)
		if (*j) etl::rects_subtract(rects, (*j)->rect);
	rects_merge(rects);

	if (rects.empty()) return false;

	// build rendering task
	canvas->set_time(id.time);
	canvas->set_outline_grow(rend_desc.get_outline_grow());
	CanvasBase sub_queue;
	Context context = canvas->get_context_sorted(context_params, sub_queue);
	rendering::Task::Handle task = context.build_rendering_task();
	sub_queue.clear();

	// add transformation task to flip result if needed
	if (task && transform) {
		rendering::TaskTransformationAffine::Handle t = new rendering::TaskTransformationAffine();
		t->transformation->matrix = matrix;
		t->sub_task() = task;
		task = t;
	}

	// TaskSurface assumed as valid non-trivial task by renderer
	// and TaskTransformationAffine of TaskSurface will not be optimized.
	// To avoid this construction place creation of dummy TaskSurface here.
	if (!task) task = new rendering::TaskSurface();

	rendering::Task::List list;
	list.push_back(task);

	for(std::vector<synfig::RectInt>::iterator j = rects.begin(); j != rects.end(); ++j) {
		// snap rect corners to tile grid
		RectInt &rect = *j;
		rect.minx = int_floor(rect.minx, tile_grid_step);
		rect.miny = int_floor(rect.miny, tile_grid_step);
		rect.maxx = int_ceil (rect.maxx, tile_grid_step);
		rect.maxy = int_ceil (rect.maxy, tile_grid_step);
		rect &= full_rect;

		RendDesc tile_desc=rend_desc;
		tile_desc.set_subwindow(rect.minx, rect.miny, rect.get_width(), rect.get_height());

		rendering::Task::Handle tile_task = task->clone_recursive();
		tile_task->target_surface = new rendering::SurfaceResource();
		tile_task->target_surface->create(tile_desc.get_w(), tile_desc.get_h());
		tile_task->target_rect = RectInt( VectorInt(), tile_task->target_surface->get_size() );
		tile_task->source_rect = Rect(tile_desc.get_tl(), tile_desc.get_br());

		Tile::Handle tile = new Tile(id, *j);
		tile->surface = tile_task->target_surface;

		tile->event = new rendering::TaskEvent();
		tile->event->signal_finished.connect( sigc::bind(
			sigc::ptr_fun(&on_tile_finished_callback), this, tile ));

		insert_tile(frame_tiles, tile);

		// Renderer::enqueue contains the expensive 'optimization' stage, so call it async
		ThreadPool::instance.enqueue( sigc::bind(
			sigc::ptr_fun(&rendering::Renderer::enqueue_task_func),
			renderer, tile_task, tile->event, false ));
	}

	return true;
}

void
Renderer_Canvas::enqueue_render()
{
	assert(get_work_area());

	rendering::Task::List events;

	{
		Glib::Threads::Mutex::Lock lock(mutex);

		String         renderer_name  = get_work_area()->get_renderer();
		RectInt        window_rect    = get_work_area()->get_window_rect();
		Canvas::Handle canvas         = get_work_area()->get_canvas();
		RendDesc       rend_desc      = canvas->rend_desc();

		build_onion_frames();

		rendering::Renderer::Handle renderer = rendering::Renderer::get_renderer(renderer_name);
		if (renderer) {
			int enqueued = 0;
			if (canvas && window_rect.is_valid()) {
				// generate rendering tasks for visible areas
				Time orig_time = canvas->get_time();
				for(FrameList::const_iterator i = onion_frames.begin(); i != onion_frames.end(); ++i)
					if (enqueue_render_frame(renderer, i->id))
						++enqueued;

				remove_extra_tiles(events);

				for(TileMap::const_iterator i = tiles.begin(); i != tiles.end(); ++i)
					for(TileList::const_iterator j = i->second.begin(); j != i->second.end(); ++j)
						if (!*j || (*j)->event)
							++enqueued;

				// generate rendering tasks for future or past frames
				int future = 0, past = 0;
				long long frame_size = image_rect_size(window_rect);
				while(tiles_size + frame_size < max_tiles_size_soft && enqueued < 1) {
					Time future_time = current_frame.time + frame_duration*future;
					bool future_exists = future_time >= rend_desc.get_time_start()
									  && future_time <= rend_desc.get_time_end();
					Time past_time = current_frame.time - frame_duration*past;
					bool past_exists = past_time >= rend_desc.get_time_start()
									&& past_time <= rend_desc.get_time_end();
					if (!future_exists && !past_exists) break;

					if (!past_exists || weight_future*future < weight_past*past) {
						// queue future
						if (enqueue_render_frame(renderer, FrameId(future_time, current_frame.width, current_frame.height)))
							++enqueued;
						++future;
					} else {
						// queue past
						if (enqueue_render_frame(renderer, FrameId(past_time, current_frame.width, current_frame.height)))
							++enqueued;
						++past;
					}
				}

				canvas->set_time(orig_time);
			}
		}
	}

	rendering::Renderer::cancel(events);
}

void
Renderer_Canvas::wait_render()
{
	rendering::TaskEvent::List events;
	{
		Glib::Threads::Mutex::Lock lock(mutex);
		for(FrameList::const_iterator i = onion_frames.begin(); i != onion_frames.end(); ++i) {
			TileMap::const_iterator ii = tiles.find(i->id);
			if (ii != tiles.end())
				for(TileList::const_iterator j = ii->second.begin(); j != ii->second.end(); ++j)
					if (*j && (*j)->event)
						events.push_back((*j)->event);
		}
	}
	for(rendering::TaskEvent::List::const_iterator i = events.begin(); i != events.end(); ++i)
		(*i)->wait();
}

void
Renderer_Canvas::clear_render()
{
	rendering::Task::List events;
	{
		Glib::Threads::Mutex::Lock lock(mutex);
		for(TileMap::iterator i = tiles.begin(); i != tiles.end(); ++i)
			while(!i->second.empty()) {
				TileList::iterator j = i->second.end(); --j;
				erase_tile(i->second, j++, events);
			}
		tiles.clear();
	}
	rendering::Renderer::cancel(events);
}

Renderer_Canvas::FrameStatus
Renderer_Canvas::calc_frame_status(const FrameId &id, const synfig::RectInt &window_rect)
{
	// mutex must be already locked

	TileMap::const_iterator i = tiles.find(id);
	if (i == tiles.end() || i->second.empty())
		return FS_None;

	std::vector<synfig::RectInt> rects;
	rects.reserve(20);
	rects.push_back(window_rect);
	for(TileList::const_iterator j = i->second.begin(); j != i->second.end(); ++j)
		if (*j) {
			if ((*j)->event)
				return FS_InProcess;
			if ((*j)->cairo_surface)
				etl::rects_subtract(rects, (*j)->rect);
		}
	rects_merge(rects);

	if (rects.size() == 1 && rects.front() == window_rect)
		return FS_None;
	if (rects.empty())
		return FS_Done;
	return FS_PartiallyDone;
}

void
Renderer_Canvas::get_render_status(StatusMap &out_map)
{
	Glib::Threads::Mutex::Lock lock(mutex);

	Canvas::Handle canvas      = get_work_area()->get_canvas();
	RectInt        window_rect = get_work_area()->get_window_rect();
	RendDesc       rend_desc   = canvas->rend_desc();

	out_map.clear();

	out_map[current_frame] = calc_frame_status(current_frame, window_rect);

	if (frame_duration) {
		int frame = (int)floor((double)(rend_desc.get_time_start() - current_frame.time)/(double)frame_duration);
		while(true) {
			Time time = current_frame.time + frame_duration*frame;
			if (time > rend_desc.get_time_end())
				break;
			if (frame && time >= rend_desc.get_time_start()) {
				FrameId id(time, current_frame.width, current_frame.height);
				out_map[id] = calc_frame_status(id, window_rect);
			}
			++frame;
		}
	}
}

void
Renderer_Canvas::render_vfunc(
	const Glib::RefPtr<Gdk::Window>& drawable,
	const Gdk::Rectangle& expose_area )
{
	draw_queued = false;

	VectorInt window_offset = get_work_area()->get_windows_offset();
	RectInt   window_rect   = get_work_area()->get_window_rect();
	RectInt   expose_rect   = RectInt( expose_area.get_x(),
			                           expose_area.get_y(),
									   expose_area.get_x() + expose_area.get_width(),
									   expose_area.get_y() + expose_area.get_height() );
	expose_rect -= window_offset;
	expose_rect &= window_rect;
	if (!expose_rect.is_valid()) return;

	// enqueue rendering if not all of visible tiles are exists and actual
	enqueue_render();

	// query frames status
	StatusMap status_map;
	get_render_status(status_map);

	Glib::Threads::Mutex::Lock lock(mutex);

	if (onion_frames.empty()) return;

	Cairo::RefPtr<Cairo::Context> context = drawable->create_cairo_context();
	context->save();
	context->translate((double)window_offset[0], (double)window_offset[1]);

	// context for tiles
	Cairo::RefPtr<Cairo::ImageSurface> onion_surface;
	Cairo::RefPtr<Cairo::Context> onion_context = context;
	if ( onion_frames.size() > 1
	  || !approximate_equal_lp(onion_frames.front().alpha, ColorReal(1.f)) )
	{
		// create surface to merge onion skin
		onion_surface = Cairo::ImageSurface::create(
			Cairo::FORMAT_ARGB32, expose_rect.get_width(), expose_rect.get_height() );
		onion_context = Cairo::Context::create(onion_surface);
		onion_context->translate(-(double)expose_rect.minx, -(double)expose_rect.miny);
		onion_context->set_operator(Cairo::OPERATOR_ADD);

		// prepare background to tune alpha
		alpha_context->set_operator(onion_context->get_operator());
		alpha_context->set_source(alpha_src_surface, 0, 0);
		int alpha_offset = FLAGS(pixel_format, PF_A_START) ? 0 : 3;
		unsigned char base[] = {0, 0, 0, 0};
		memcpy(alpha_dst_surface->get_data(), base, sizeof(base));
		alpha_dst_surface->mark_dirty();
		alpha_dst_surface->flush();
		for(FrameList::const_iterator j = onion_frames.begin(), i = j++; j != onion_frames.end(); i = j++)
			alpha_context->paint_with_alpha(i->alpha);
		alpha_dst_surface->flush();
		memcpy(base, alpha_dst_surface->get_data(), sizeof(base));

		// tune alpha
		while(true) {
			memcpy(alpha_dst_surface->get_data(), base, sizeof(base));
			alpha_dst_surface->mark_dirty();
			alpha_dst_surface->flush();
			alpha_context->paint_with_alpha(onion_frames.back().alpha);
			int alpha = alpha_dst_surface->get_data()[alpha_offset];
			if (alpha >= 255) break;
			onion_frames.back().alpha += (ColorReal)(255 - alpha)/ColorReal(128.f);
		}
	}

	// draw tiles
	onion_context->save();
	for(FrameList::const_iterator i = onion_frames.begin(); i != onion_frames.end(); ++i) {
		TileMap::const_iterator ii = tiles.find(i->id);
		if (ii == tiles.end()) continue;
		for(TileList::const_iterator j = ii->second.begin(); j != ii->second.end(); ++j) {
			if (!*j) continue;
			if ((*j)->cairo_surface) {
				onion_context->save();
				onion_context->rectangle((*j)->rect.minx, (*j)->rect.miny, (*j)->rect.get_width(), (*j)->rect.get_height());
				onion_context->clip();
				onion_context->set_source((*j)->cairo_surface, (*j)->rect.minx, (*j)->rect.miny);
				if (onion_surface)
					onion_context->paint_with_alpha(i->alpha);
				else
					onion_context->paint();
				onion_context->restore();
			}
		}
	}
	onion_context->restore();

	// finish with onion skin
	if (onion_surface) {
		assert(onion_context != context);
		onion_surface->flush();

		// put merged onion to context
		context->save();
		context->set_source(onion_surface, (double)expose_rect.minx, (double)expose_rect.miny);
		context->paint();
		context->restore();
	}

	// draw the border around the rendered region
	context->save();
	context->set_line_cap(Cairo::LINE_CAP_BUTT);
	context->set_line_join(Cairo::LINE_JOIN_MITER);
	context->set_antialias(Cairo::ANTIALIAS_NONE);
	context->set_line_width(1.0);
	context->set_source_rgba(0.0, 0.0, 0.0, 1.0);
	context->rectangle(0.0, 0.0, (double)current_frame.width, (double)current_frame.height);
	context->stroke();
	context->restore();

	// draw frames status
	if (!status_map.empty()) {
		context->save();
		context->translate(0.0, (double)current_frame.height);
		double scale = (double)current_frame.width/(double)status_map.size();
		context->scale(scale, scale);
		for(StatusMap::const_iterator i = status_map.begin(); i != status_map.end(); ++i) {
			switch(i->second) {
			case FS_PartiallyDone:
				context->set_source_rgba(0.5, 0.5, 0.5, 1.0); break;
			case FS_InProcess:
				context->set_source_rgba(1.0, 1.0, 0.0, 1.0); break;
			case FS_Done:
				context->set_source_rgba(0.0, 0.0, 0.0, 1.0); break;
			default:
				context->set_source_rgba(1.0, 1.0, 1.0, 1.0); break;
			}
			context->rectangle(0.0, 0.0, 1.0, 1.0);
			context->fill();
			context->translate(1.0, 0.0);
		}
		context->restore();
	}

	context->restore();
}
