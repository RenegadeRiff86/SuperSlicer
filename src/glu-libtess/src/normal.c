/*
 * SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
 * Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice including the dates of first publication and
 * either this permission notice or a reference to
 * http://oss.sgi.com/projects/FreeB/
 * shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * SILICON GRAPHICS, INC. BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of Silicon Graphics, Inc.
 * shall not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization from
 * Silicon Graphics, Inc.
 */
/*
** Author: Eric Veach, July 1994.
**
*/

#include "gluos.h"
#include "mesh.h"
#include "tess.h"
#include "normal.h"
#include <math.h>
#include <assert.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

enum {
  TESS_COORD_X_INDEX,
  TESS_COORD_Y_INDEX,
  TESS_COORD_Z_INDEX,
  TESS_COORD_COUNT
};

enum {
  TESS_AXIS_NEXT_OFFSET = TESS_COORD_Y_INDEX,
  TESS_AXIS_SECOND_NEXT_OFFSET = TESS_COORD_Z_INDEX,
  TESS_BOUND_EXTENT_MULTIPLIER = TESS_COORD_Z_INDEX
};

#define Dot(u,v)	(u[TESS_COORD_X_INDEX]*v[TESS_COORD_X_INDEX] + u[TESS_COORD_Y_INDEX]*v[TESS_COORD_Y_INDEX] + u[TESS_COORD_Z_INDEX]*v[TESS_COORD_Z_INDEX])

#if 0
static void Normalize( GLdouble v[TESS_COORD_COUNT] )
{
  GLdouble len = v[TESS_COORD_X_INDEX]*v[TESS_COORD_X_INDEX] + v[TESS_COORD_Y_INDEX]*v[TESS_COORD_Y_INDEX] + v[TESS_COORD_Z_INDEX]*v[TESS_COORD_Z_INDEX];

  assert( len > 0 );
  len = sqrt( len );
  v[TESS_COORD_X_INDEX] /= len;
  v[TESS_COORD_Y_INDEX] /= len;
  v[TESS_COORD_Z_INDEX] /= len;
}
#endif

#undef	ABS
#define ABS(x)	((x) < 0 ? -(x) : (x))

static int LongAxis( GLdouble v[TESS_COORD_COUNT] )
{
  int i = 0;

  if( ABS(v[TESS_COORD_Y_INDEX]) > ABS(v[TESS_COORD_X_INDEX]) ) { i = TESS_COORD_Y_INDEX; }
  if( ABS(v[TESS_COORD_Z_INDEX]) > ABS(v[i]) ) { i = TESS_COORD_Z_INDEX; }
  return i;
}

static void ComputeNormal( GLUtesselator *tess, GLdouble norm[TESS_COORD_COUNT] )
{
  GLUvertex *v = NULL, *v1 = NULL, *v2 = NULL;
  GLdouble c = 0, tLen2 = 0, maxLen2 = 0;
  GLdouble maxVal[TESS_COORD_COUNT] = { 0 }, minVal[TESS_COORD_COUNT] = { 0 }, d1[TESS_COORD_COUNT] = { 0 }, d2[TESS_COORD_COUNT] = { 0 }, tNorm[TESS_COORD_COUNT] = { 0 };
  GLUvertex *maxVert[TESS_COORD_COUNT] = { NULL, NULL, NULL }, *minVert[TESS_COORD_COUNT] = { NULL, NULL, NULL };
  GLUvertex *vHead = &tess->mesh->vHead;
  int i;

  maxVal[TESS_COORD_X_INDEX] = maxVal[TESS_COORD_Y_INDEX] = maxVal[TESS_COORD_Z_INDEX] = -TESS_BOUND_EXTENT_MULTIPLIER * GLU_TESS_MAX_COORD;
  minVal[TESS_COORD_X_INDEX] = minVal[TESS_COORD_Y_INDEX] = minVal[TESS_COORD_Z_INDEX] = TESS_BOUND_EXTENT_MULTIPLIER * GLU_TESS_MAX_COORD;

  for( v = vHead->next; v != vHead; v = v->next ) {
    for( i = 0; i < TESS_COORD_COUNT; ++i ) {
      c = v->coords[i];
      if( c < minVal[i] ) { minVal[i] = c; minVert[i] = v; }
      if( c > maxVal[i] ) { maxVal[i] = c; maxVert[i] = v; }
    }
  }

  /* Find two vertices separated by at least 1/sqrt(3) of the maximum
   * distance between any two vertices
   */
  i = 0;
  if( maxVal[TESS_COORD_Y_INDEX] - minVal[TESS_COORD_Y_INDEX] > maxVal[TESS_COORD_X_INDEX] - minVal[TESS_COORD_X_INDEX] ) { i = 1; }
  if( maxVal[TESS_COORD_Z_INDEX] - minVal[TESS_COORD_Z_INDEX] > maxVal[i] - minVal[i] ) { i = 2; }
  if( minVal[i] >= maxVal[i] ) {
    /* All vertices are the same -- normal doesn't matter */
    norm[TESS_COORD_X_INDEX] = 0; norm[TESS_COORD_Y_INDEX] = 0; norm[TESS_COORD_Z_INDEX] = 1;
    return;
  }

  /* Look for a third vertex which forms the triangle with maximum area
   * (Length of normal == twice the triangle area)
   */
  maxLen2 = 0;
  v1 = minVert[i];
  v2 = maxVert[i];
  d1[TESS_COORD_X_INDEX] = v1->coords[TESS_COORD_X_INDEX] - v2->coords[TESS_COORD_X_INDEX];
  d1[TESS_COORD_Y_INDEX] = v1->coords[TESS_COORD_Y_INDEX] - v2->coords[TESS_COORD_Y_INDEX];
  d1[TESS_COORD_Z_INDEX] = v1->coords[TESS_COORD_Z_INDEX] - v2->coords[TESS_COORD_Z_INDEX];
  for( v = vHead->next; v != vHead; v = v->next ) {
    d2[TESS_COORD_X_INDEX] = v->coords[TESS_COORD_X_INDEX] - v2->coords[TESS_COORD_X_INDEX];
    d2[TESS_COORD_Y_INDEX] = v->coords[TESS_COORD_Y_INDEX] - v2->coords[TESS_COORD_Y_INDEX];
    d2[TESS_COORD_Z_INDEX] = v->coords[TESS_COORD_Z_INDEX] - v2->coords[TESS_COORD_Z_INDEX];
    tNorm[TESS_COORD_X_INDEX] = d1[TESS_COORD_Y_INDEX]*d2[TESS_COORD_Z_INDEX] - d1[TESS_COORD_Z_INDEX]*d2[TESS_COORD_Y_INDEX];
    tNorm[TESS_COORD_Y_INDEX] = d1[TESS_COORD_Z_INDEX]*d2[TESS_COORD_X_INDEX] - d1[TESS_COORD_X_INDEX]*d2[TESS_COORD_Z_INDEX];
    tNorm[TESS_COORD_Z_INDEX] = d1[TESS_COORD_X_INDEX]*d2[TESS_COORD_Y_INDEX] - d1[TESS_COORD_Y_INDEX]*d2[TESS_COORD_X_INDEX];
    tLen2 = tNorm[TESS_COORD_X_INDEX]*tNorm[TESS_COORD_X_INDEX] + tNorm[TESS_COORD_Y_INDEX]*tNorm[TESS_COORD_Y_INDEX] + tNorm[TESS_COORD_Z_INDEX]*tNorm[TESS_COORD_Z_INDEX];
    if( tLen2 > maxLen2 ) {
      maxLen2 = tLen2;
      norm[TESS_COORD_X_INDEX] = tNorm[TESS_COORD_X_INDEX];
      norm[TESS_COORD_Y_INDEX] = tNorm[TESS_COORD_Y_INDEX];
      norm[TESS_COORD_Z_INDEX] = tNorm[TESS_COORD_Z_INDEX];
    }
  }

  if( maxLen2 <= 0 ) {
    /* All points lie on a single line -- any decent normal will do */
    norm[TESS_COORD_X_INDEX] = norm[TESS_COORD_Y_INDEX] = norm[TESS_COORD_Z_INDEX] = 0;
    norm[LongAxis(d1)] = 1;
  }
}


static void CheckOrientation( GLUtesselator *tess )
{
  GLdouble area;
  GLUface *f, *fHead = &tess->mesh->fHead;
  GLUvertex *v, *vHead = &tess->mesh->vHead;
  GLUhalfEdge *e;

  /* When we compute the normal automatically, we choose the orientation
   * so that the sum of the signed areas of all contours is non-negative.
   */
  area = 0;
  for( f = fHead->next; f != fHead; f = f->next ) {
    e = f->anEdge;
    if( e->winding <= 0 ) continue;
    do {
      area += (e->Org->s - e->Dst->s) * (e->Org->t + e->Dst->t);
      e = e->Lnext;
    } while( e != f->anEdge );
  }
  if( area < 0 ) {
    /* Reverse the orientation by flipping all the t-coordinates */
    for( v = vHead->next; v != vHead; v = v->next ) {
      v->t = - v->t;
    }
    tess->tUnit[TESS_COORD_X_INDEX] = - tess->tUnit[TESS_COORD_X_INDEX];
    tess->tUnit[TESS_COORD_Y_INDEX] = - tess->tUnit[TESS_COORD_Y_INDEX];
    tess->tUnit[TESS_COORD_Z_INDEX] = - tess->tUnit[TESS_COORD_Z_INDEX];
  }
}

#ifdef FOR_TRITE_TEST_PROGRAM
#include <stdlib.h>
extern int RandomSweep;
#define S_UNIT_X	(RandomSweep ? (TESS_BOUND_EXTENT_MULTIPLIER*drand48()-1) : 1.0)
#define S_UNIT_Y	(RandomSweep ? (TESS_BOUND_EXTENT_MULTIPLIER*drand48()-1) : 0.0)
#else
#if defined(SLANTED_SWEEP)
/* The "feature merging" is not intended to be complete.  There are
 * special cases where edges are nearly parallel to the sweep line
 * which are not implemented.  The algorithm should still behave
 * robustly (ie. produce a reasonable tesselation) in the presence
 * of such edges, however it may miss features which could have been
 * merged.  We could minimize this effect by choosing the sweep line
 * direction to be something unusual (ie. not parallel to one of the
 * coordinate axes).
 */
#define S_UNIT_X	0.50941539564955385	/* Pre-normalized */
#define S_UNIT_Y	0.86052074622010633
#else
#define S_UNIT_X	1.0
#define S_UNIT_Y	0.0
#endif
#endif

/* Determine the polygon normal and project vertices onto the plane
 * of the polygon.
 */
void __gl_projectPolygon( GLUtesselator *tess )
{
  GLUvertex *v, *vHead = &tess->mesh->vHead;
  GLdouble norm[TESS_COORD_COUNT] = { 0 };
  GLdouble *sUnit = NULL, *tUnit = NULL;
  int i = 0, computedNormal = FALSE;

  norm[TESS_COORD_X_INDEX] = tess->normal[TESS_COORD_X_INDEX];
  norm[TESS_COORD_Y_INDEX] = tess->normal[TESS_COORD_Y_INDEX];
  norm[TESS_COORD_Z_INDEX] = tess->normal[TESS_COORD_Z_INDEX];
  if( norm[TESS_COORD_X_INDEX] == 0 && norm[TESS_COORD_Y_INDEX] == 0 && norm[TESS_COORD_Z_INDEX] == 0 ) {
    ComputeNormal( tess, norm );
    computedNormal = TRUE;
  }
  sUnit = tess->sUnit;
  tUnit = tess->tUnit;
  i = LongAxis( norm );

#if defined(FOR_TRITE_TEST_PROGRAM) || defined(TRUE_PROJECT)
  /* Choose the initial sUnit vector to be approximately perpendicular
   * to the normal.
   */
  Normalize( norm );

  sUnit[i] = 0;
  sUnit[(i+TESS_AXIS_NEXT_OFFSET)%TESS_COORD_COUNT] = S_UNIT_X;
  sUnit[(i+TESS_AXIS_SECOND_NEXT_OFFSET)%TESS_COORD_COUNT] = S_UNIT_Y;

  /* Now make it exactly perpendicular */
  w = Dot( sUnit, norm );
  sUnit[TESS_COORD_X_INDEX] -= w * norm[TESS_COORD_X_INDEX];
  sUnit[TESS_COORD_Y_INDEX] -= w * norm[TESS_COORD_Y_INDEX];
  sUnit[TESS_COORD_Z_INDEX] -= w * norm[TESS_COORD_Z_INDEX];
  Normalize( sUnit );

  /* Choose tUnit so that (sUnit,tUnit,norm) form a right-handed frame */
  tUnit[TESS_COORD_X_INDEX] = norm[TESS_COORD_Y_INDEX]*sUnit[TESS_COORD_Z_INDEX] - norm[TESS_COORD_Z_INDEX]*sUnit[TESS_COORD_Y_INDEX];
  tUnit[TESS_COORD_Y_INDEX] = norm[TESS_COORD_Z_INDEX]*sUnit[TESS_COORD_X_INDEX] - norm[TESS_COORD_X_INDEX]*sUnit[TESS_COORD_Z_INDEX];
  tUnit[TESS_COORD_Z_INDEX] = norm[TESS_COORD_X_INDEX]*sUnit[TESS_COORD_Y_INDEX] - norm[TESS_COORD_Y_INDEX]*sUnit[TESS_COORD_X_INDEX];
  Normalize( tUnit );
#else
  /* Project perpendicular to a coordinate axis -- better numerically */
  sUnit[i] = 0;
  sUnit[(i+TESS_AXIS_NEXT_OFFSET)%TESS_COORD_COUNT] = S_UNIT_X;
  sUnit[(i+TESS_AXIS_SECOND_NEXT_OFFSET)%TESS_COORD_COUNT] = S_UNIT_Y;

  tUnit[i] = 0;
  tUnit[(i+TESS_AXIS_NEXT_OFFSET)%TESS_COORD_COUNT] = (norm[i] > 0) ? -S_UNIT_Y : S_UNIT_Y;
  tUnit[(i+TESS_AXIS_SECOND_NEXT_OFFSET)%TESS_COORD_COUNT] = (norm[i] > 0) ? S_UNIT_X : -S_UNIT_X;
#endif

  /* Project the vertices onto the sweep plane */
  for( v = vHead->next; v != vHead; v = v->next ) {
    v->s = Dot( v->coords, sUnit );
    v->t = Dot( v->coords, tUnit );
  }
  if( computedNormal ) {
    CheckOrientation( tess );
  }
}
