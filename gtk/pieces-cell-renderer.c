/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2005-2008 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <libtransmission/transmission.h>
#include <libtransmission/utils.h>
#include "pieces-cell-renderer.h"
#include "tr-torrent.h"
#include "util.h"

enum
{
    PROP_0,
    PROP_TORRENT
};

static const gint MIN_BAR_WIDTH   = 100;
static const gint MIN_BAR_HEIGHT  = 20;
static const gint PROGRESS_HEIGHT = 4;
static const gint BORDER_WIDTH    = 1;

/* See pieces_cell_renderer_class_init() for color values. */

typedef struct _PiecesCellRendererClassPrivate
{
    GdkColor piece_bg_color;
    GdkColor piece_have_color;
    GdkColor piece_missing_color;
    GdkColor piece_seeding_color;
    GdkColor progress_bg_color;
    GdkColor progress_bar_color;
    GdkColor ratio_bg_color;
    GdkColor ratio_bar_color;
    GdkColor border_color;
    GdkColor paused_bar_color;
} PiecesCellRendererClassPrivate;

static PiecesCellRendererClassPrivate cpriv_data;
static PiecesCellRendererClassPrivate * cpriv = &cpriv_data;

struct _PiecesCellRendererPrivate
{
    TrTorrent       * gtor;
    cairo_surface_t * offscreen;
    int               offscreen_w;
    int               offscreen_h;
};

static void
pieces_cell_renderer_get_size( GtkCellRenderer * cell,
                               GtkWidget       * widget UNUSED,
                               GdkRectangle    * cell_area,
                               gint            * x_offset,
                               gint            * y_offset,
                               gint            * width,
                               gint            * height )
{
    if( width )
        *width = MIN_BAR_WIDTH + cell->xpad * 2;
    if( height )
        *height = MIN_BAR_HEIGHT + cell->ypad * 2;

    if( cell_area )
    {
        if( width )
            *width = cell_area->width;
        if( height )
            *height = cell_area->height;
    }

    if( x_offset )
        *x_offset = 0;
    if( y_offset )
        *y_offset = 0;
}

static cairo_t *
get_offscreen_context( PiecesCellRendererPrivate * priv,
                       cairo_t * cr, int w, int h )
{
    if( !priv->offscreen || priv->offscreen_w != w
        || priv->offscreen_h != h )
    {
        cairo_surface_t * target = cairo_get_target( cr ), * s;
        if( priv->offscreen )
            cairo_surface_destroy( priv->offscreen );
        s = cairo_surface_create_similar( target,
                                          CAIRO_CONTENT_COLOR_ALPHA,
                                          w, h );
        priv->offscreen = s;
        priv->offscreen_w = w;
        priv->offscreen_h = h;
    }
    return cairo_create( priv->offscreen );
}

static void
render_progress( PiecesCellRendererPrivate * priv,
                 cairo_t * cr, int x, int y, int w, int h )
{
    const tr_stat * st = tr_torrent_stat( priv->gtor );
    GdkColor * bg_color, * bar_color;
    double progress;

    if( !st )
    {
        gdk_cairo_set_source_color( cr, &cpriv->progress_bg_color );
        cairo_rectangle( cr, x, y, w, h );
        cairo_fill( cr );
        return;
    }

    if( st->percentDone >= 1.0 )
    {
        progress = MIN( 1.0, MAX( 0.0, st->seedRatioPercentDone ) );
        bg_color = &cpriv->ratio_bg_color;
        bar_color = &cpriv->ratio_bar_color;
    }
    else
    {
        progress = MIN( 1.0, MAX( 0.0, st->percentDone ) );
        bg_color = &cpriv->progress_bg_color;
        bar_color = &cpriv->progress_bar_color;
    }

    if( st->activity == TR_STATUS_STOPPED )
    {
        bg_color = &cpriv->progress_bg_color;
        bar_color = &cpriv->paused_bar_color;
    }

    if( progress < 1.0 )
    {
        gdk_cairo_set_source_color( cr, bg_color );
        cairo_rectangle( cr, x, y, w, h );
        cairo_fill( cr );
    }

    if( progress > 0.0 )
    {
        gdk_cairo_set_source_color( cr, bar_color );
        cairo_rectangle( cr, x, y, progress * w, h );
        cairo_fill( cr );
    }
}

static void
render_pieces( PiecesCellRendererPrivate * priv,
               cairo_t * cr, int x, int y, int w, int h )
{
    const tr_stat * st;
    const int8_t * avtab;

    if (w < 1 || h < 1)
        return;

    gdk_cairo_set_source_color( cr, &cpriv->piece_bg_color );
    cairo_rectangle( cr, x, y, w, h );
    cairo_fill( cr );

    st = tr_torrent_stat( priv->gtor );
    avtab = tr_torrent_availability( priv->gtor, w );
    if( st && avtab )
    {
        const tr_bool connected = ( st->peersConnected > 0 );
        const tr_bool seeding = ( st->percentDone >= 1.0 );
        GdkColor * piece_have_color;
        GdkColor * piece_missing_color;
        int i, j;
        int8_t avail;

        if( seeding )
            piece_have_color = &cpriv->piece_seeding_color;
        else
            piece_have_color = &cpriv->piece_have_color;

        if( connected )
            piece_missing_color = &cpriv->piece_missing_color;
        else
            piece_missing_color = &cpriv->piece_bg_color;

        for( i = 0; i < w; )
        {
            if( avtab[i] > 0 )
            {
                ++i;
                continue;
            }
            avail = avtab[i];
            for( j = i + 1; j < w; ++j )
                if( avtab[j] != avail )
                    break;
            if( avail == 0 )
                gdk_cairo_set_source_color( cr, piece_missing_color );
            else
                gdk_cairo_set_source_color( cr, piece_have_color );
            cairo_rectangle( cr, x + i, y, j - i, h );
            cairo_fill( cr );
            i = j;
        }
    }
}

static void
pieces_cell_renderer_render( GtkCellRenderer      * cell,
                             GdkDrawable          * window,
                             GtkWidget            * widget UNUSED,
                             GdkRectangle         * background_area UNUSED,
                             GdkRectangle         * cell_area,
                             GdkRectangle         * expose_area,
                             GtkCellRendererState   flags UNUSED )
{
    PiecesCellRenderer * self = PIECES_CELL_RENDERER( cell );
    PiecesCellRendererPrivate * priv = self->priv;
    gint x, y, w, h, xo, yo, wo, ho, hp;
    cairo_t * cr, * cro;

    x = cell_area->x + cell->xpad;
    y = cell_area->y + cell->ypad;
    w = cell_area->width - cell->xpad * 2;
    h = cell_area->height - cell->ypad * 2;

    cr = gdk_cairo_create( window );

    gdk_cairo_rectangle( cr, expose_area );
    cairo_clip( cr );

    cro = get_offscreen_context( priv, cr, w, h );
    xo = 0;
    yo = 0;
    wo = w;
    ho = h;

    gdk_cairo_set_source_color( cro, &cpriv->border_color );
    cairo_paint( cro );
    xo += BORDER_WIDTH;
    yo += BORDER_WIDTH;
    wo -= 2 * BORDER_WIDTH;
    ho -= 2 * BORDER_WIDTH;
    hp = PROGRESS_HEIGHT;

    render_progress( priv, cro, xo, yo, wo, hp );
    render_pieces( priv, cro, xo, yo + hp, wo, ho - hp );
    cairo_destroy( cro );

    cairo_set_source_surface( cr, priv->offscreen, x, y );
    cairo_paint( cr );

    cairo_destroy( cr );
}

static void
pieces_cell_renderer_set_property( GObject      * object,
                                   guint          property_id,
                                   const GValue * v,
                                   GParamSpec   * pspec )
{
    PiecesCellRenderer * self = PIECES_CELL_RENDERER( object );
    PiecesCellRendererPrivate * priv = self->priv;

    switch( property_id )
    {
        case PROP_TORRENT:
            priv->gtor = g_value_get_object( v );
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID( object, property_id, pspec );
            break;
    }
}

static void
pieces_cell_renderer_get_property( GObject    * object,
                                   guint        property_id,
                                   GValue     * v,
                                   GParamSpec * pspec )
{
    const PiecesCellRenderer * self = PIECES_CELL_RENDERER( object );
    PiecesCellRendererPrivate * priv = self->priv;

    switch( property_id )
    {
        case PROP_TORRENT:
            g_value_set_object( v, priv->gtor );
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID( object, property_id, pspec );
            break;
    }
}

static void
pieces_cell_renderer_finalize( GObject * object )
{
    PiecesCellRenderer * self = PIECES_CELL_RENDERER( object );
    PiecesCellRendererPrivate * priv = self->priv;
    GObjectClass * parent;

    if( priv->offscreen )
    {
        cairo_surface_destroy( priv->offscreen );
        priv->offscreen = NULL;
    }

    parent = g_type_class_peek( g_type_parent( PIECES_CELL_RENDERER_TYPE ) ) ;
    parent->finalize( object );
}

static void
pieces_cell_renderer_class_init( PiecesCellRendererClass * klass )
{
    GObjectClass * gobject_class = G_OBJECT_CLASS( klass );
    GtkCellRendererClass * cell_class = GTK_CELL_RENDERER_CLASS( klass );

    g_type_class_add_private( klass, sizeof( PiecesCellRendererPrivate ) );

    cell_class->render          = pieces_cell_renderer_render;
    cell_class->get_size        = pieces_cell_renderer_get_size;
    gobject_class->set_property = pieces_cell_renderer_set_property;
    gobject_class->get_property = pieces_cell_renderer_get_property;
    gobject_class->finalize     = pieces_cell_renderer_finalize;

    g_object_class_install_property( gobject_class, PROP_TORRENT,
                                     g_param_spec_object( "torrent", NULL,
                                                          "TrTorrent*",
                                                          TR_TORRENT_TYPE,
                                                          G_PARAM_READWRITE ) );

    gdk_color_parse( "#efefff", &cpriv->piece_bg_color );
    gdk_color_parse( "#2975d6", &cpriv->piece_have_color );
    gdk_color_parse( "#d90000", &cpriv->piece_missing_color );
    gdk_color_parse( "#30b027", &cpriv->piece_seeding_color );
    gdk_color_parse( "#dadada", &cpriv->progress_bg_color );
    gdk_color_parse( "#314e6c", &cpriv->progress_bar_color );
    gdk_color_parse( "#a6e3b4", &cpriv->ratio_bg_color );
    gdk_color_parse( "#448632", &cpriv->ratio_bar_color );
    gdk_color_parse( "#888888", &cpriv->border_color );
    gdk_color_parse( "#aaaaaa", &cpriv->paused_bar_color );
}

static void
pieces_cell_renderer_init( GTypeInstance * instance,
                           gpointer g_class UNUSED )
{
    PiecesCellRenderer * self = PIECES_CELL_RENDERER( instance );
    PiecesCellRendererPrivate * priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE(self, PIECES_CELL_RENDERER_TYPE,
                                       PiecesCellRendererPrivate );
    priv->gtor = NULL;
    priv->offscreen = NULL;

    self->priv = priv;
}

GType
pieces_cell_renderer_get_type( void )
{
    static GType type = 0;

    if( !type )
    {
        static const GTypeInfo info =
        {
            sizeof( PiecesCellRendererClass ),
            NULL, /* base_init */
            NULL, /* base_finalize */
            (GClassInitFunc) pieces_cell_renderer_class_init,
            NULL, /* class_finalize */
            NULL, /* class_data */
            sizeof( PiecesCellRenderer ),
            0, /* n_preallocs */
            (GInstanceInitFunc) pieces_cell_renderer_init,
            NULL
        };

        type = g_type_register_static( GTK_TYPE_CELL_RENDERER,
                                       "PiecesCellRenderer",
                                       &info, (GTypeFlags) 0 );
    }

    return type;
}

GtkCellRenderer *
pieces_cell_renderer_new( void )
{
    return g_object_new( PIECES_CELL_RENDERER_TYPE, NULL );
}
