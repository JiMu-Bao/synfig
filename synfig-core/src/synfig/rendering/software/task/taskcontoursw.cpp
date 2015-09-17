/* === S Y N F I G ========================================================= */
/*!	\file synfig/rendering/software/task/taskcontoursw.cpp
**	\brief TaskContourSW
**
**	$Id$
**
**	\legal
**	......... ... 2015 Ivan Mahonin
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

#ifndef WIN32
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#endif

#include "taskcontoursw.h"

#include "../surfacesw.h"

#include <synfig/debug/debugsurface.h>

#endif

using namespace synfig;
using namespace rendering;

/* === M A C R O S ========================================================= */

/* === G L O B A L S ======================================================= */

/* === P R O C E D U R E S ================================================= */

/* === M E T H O D S ======================================================= */

void
TaskContourSW::render_polyspan(
	synfig::Surface &target_surface,
	const Polyspan &polyspan,
	bool invert,
	bool antialias,
	Contour::WindingStyle winding_style,
	const Color &color,
	Color::value_type opacity,
	Color::BlendMethod blend_method )
{
	synfig::Surface::alpha_pen p(target_surface.begin(), opacity, blend_method);
	const Polyspan::ContextRect &window = polyspan.get_window();
	const Polyspan::cover_array &covers = polyspan.get_covers();

	Polyspan::cover_array::const_iterator cur_mark = covers.begin();
	Polyspan::cover_array::const_iterator end_mark = covers.end();

	Real cover = 0, area = 0, alpha = 0;
	int	y = 0, x = 0;

	p.set_value(color);
	cover = 0;

	if (cur_mark == end_mark)
	{
		// no marks at all
		if (invert)
		{
			p.move_to(window.minx, window.miny);
			p.put_block(window.maxy - window.miny, window.maxx - window.minx);
		}
		return;
	}

	// fill initial rect / line
	if (invert)
	{
		// fill all the area above the first vertex
		p.move_to(window.minx, window.miny);
		y = window.miny;
		int l = window.maxx - window.minx;

		p.put_block(cur_mark->y - window.miny, l);

		// fill the area to the left of the first vertex on that line
		l = cur_mark->x - window.minx;
		p.move_to(window.minx, cur_mark->y);
		if (l) p.put_hline(l);
	}

	while(true)
	{
		y = cur_mark->y;
		x = cur_mark->x;

		p.move_to(x,y);

		area = cur_mark->area;
		cover += cur_mark->cover;

		// accumulate for the current pixel
		while(++cur_mark != covers.end())
		{
			if (y != cur_mark->y || x != cur_mark->x)
				break;

			area += cur_mark->area;
			cover += cur_mark->cover;
		}

		// draw pixel - based on covered area
		if (area) // if we're ok, draw the current pixel
		{
			alpha = polyspan.extract_alpha(cover - area, winding_style);
			if (invert) alpha = 1 - alpha;

			if (antialias)
			{
				if (alpha) p.put_value_alpha(alpha);
			}
			else
			{
				if (alpha >= .5) p.put_value();
			}

			p.inc_x();
			x++;
		}

		// if we're done, don't use iterator and exit
		if (cur_mark == end_mark) break;

		// if there is no more live pixels on this line, goto next
		if (y != cur_mark->y)
		{
			if (invert)
			{
				// fill the area at the end of the line
				p.put_hline(window.maxx - x);

				// fill area at the beginning of the next line
				p.move_to(window.minx, cur_mark->y);
				p.put_hline(cur_mark->x - window.minx);
			}

			cover = 0;
			continue;
		}

		// draw span to next pixel - based on total amount of pixel cover
		if (x < cur_mark->x)
		{
			alpha = polyspan.extract_alpha(cover, winding_style);
			if (invert) alpha = 1 - alpha;

			if (antialias)
			{
				if (alpha) p.put_hline(cur_mark->x - x, alpha);
			}
			else
			{
				if (alpha >= .5) p.put_hline(cur_mark->x - x);
			}
		}
	}

	// fill the after stuff
	if (invert)
	{
		//fill the area at the end of the line
		p.put_hline(window.maxx - x);

		//fill area at the beginning of the next line
		p.move_to(window.minx, y+1);
		p.put_block(window.maxy - y - 1, window.maxx - window.minx);
	}
}

void
TaskContourSW::build_polyspan(
	const Contour::ChunkList &chunks,
	const Matrix &transform_matrix,
	Polyspan &out_polyspan )
{
	Vector p1, pp0, pp1;
	for(Contour::ChunkList::const_iterator i = chunks.begin(); i != chunks.end(); ++i)
	{
		switch(i->type)
		{
			case Contour::CLOSE:
				out_polyspan.close();
				break;
			case Contour::MOVE:
				p1 = transform_matrix.get_transformed(i->p1);
				out_polyspan.move_to(p1[0], p1[1]);
				break;
			case Contour::LINE:
				p1 = transform_matrix.get_transformed(i->p1);
				out_polyspan.line_to(p1[0], p1[1]);
				break;
			case Contour::CONIC:
				p1 = transform_matrix.get_transformed(i->p1);
				pp0 = transform_matrix.get_transformed(i->pp0);
				out_polyspan.conic_to(p1[0], p1[1], pp0[0], pp0[1]);
				break;
			case Contour::CUBIC:
				p1 = transform_matrix.get_transformed(i->p1);
				pp0 = transform_matrix.get_transformed(i->pp0);
				pp1 = transform_matrix.get_transformed(i->pp1);
				out_polyspan.cubic_to(p1[0], p1[1], pp0[0], pp0[1], pp1[0], pp1[1]);
				break;
			default:
				break;
		}
	}
}


void
TaskContourSW::render_contour(
	synfig::Surface &target_surface,
	const Contour::ChunkList &chunks,
	bool invert,
	bool antialias,
	Contour::WindingStyle winding_style,
	const Matrix &transform_matrix,
	const Color &color,
	Color::value_type opacity,
	Color::BlendMethod blend_method )
{
	Polyspan polyspan;
	polyspan.init(0, 0, target_surface.get_w(), target_surface.get_h());
	build_polyspan(chunks, transform_matrix, polyspan);
	polyspan.sort_marks();

	return render_polyspan(
		target_surface,
		polyspan,
		invert,
		antialias,
		winding_style,
		color,
		opacity,
		blend_method );
}

bool
TaskContourSW::run(RunParams &params) const
{
	synfig::Surface &a =
		SurfaceSW::Handle::cast_dynamic( target_surface )->get_surface();

	Matrix bounds_transfromation;
	bounds_transfromation.m00 = get_pixels_per_unit()[0];
	bounds_transfromation.m11 = get_pixels_per_unit()[1];
	bounds_transfromation.m20 = -rect_lt[0] * bounds_transfromation.m00;
	bounds_transfromation.m21 = -rect_lt[1] * bounds_transfromation.m11;

	Matrix matrix = transformation * bounds_transfromation;

	Polyspan polyspan;
	polyspan.init(0, 0, a.get_w(), a.get_h());
	build_polyspan(contour->get_chunks(), matrix, polyspan);
	polyspan.sort_marks();

	render_polyspan(
		a,
		polyspan,
		contour->invert,
		contour->antialias,
		contour->winding_style,
		contour->color,
		blend ? amount : 1.0,
		blend ? blend_method : Color::BLEND_COMPOSITE );

	if (!contour->invert)
		params.used_rect = polyspan.calc_bounds();

	//debug::DebugSurface::save_to_file(a, "TaskContourSW__run");

	return true;
}

/* === E N T R Y P O I N T ================================================= */
