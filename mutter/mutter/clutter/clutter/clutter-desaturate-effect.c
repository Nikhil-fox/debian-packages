/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2010  Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author:
 *   Emmanuele Bassi <ebassi@linux.intel.com>
 */

/**
 * ClutterDesaturateEffect:
 * 
 * A desaturation effect
 *
 * #ClutterDesaturateEffect is a sub-class of #ClutterEffect that
 * desaturates the color of an actor and its contents. The strength
 * of the desaturation effect is controllable and animatable through
 * the #ClutterDesaturateEffect:factor property.
 */

#define CLUTTER_DESATURATE_EFFECT_CLASS(klass)        (G_TYPE_CHECK_CLASS_CAST ((klass), CLUTTER_TYPE_DESATURATE_EFFECT, ClutterDesaturateEffectClass))
#define CLUTTER_IS_DESATURATE_EFFECT_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE ((klass), CLUTTER_TYPE_DESATURATE_EFFECT))
#define CLUTTER_DESATURATE_EFFECT_GET_CLASS(obj)      (G_TYPE_INSTANCE_GET_CLASS ((obj), CLUTTER_TYPE_DESATURATE_EFFECT, ClutterDesaturateEffectClass))

#include "clutter/clutter-build-config.h"

#include <math.h>

#include "clutter/clutter-desaturate-effect.h"

#include "cogl/cogl.h"

#include "clutter/clutter-debug.h"
#include "clutter/clutter-enum-types.h"
#include "clutter/clutter-offscreen-effect.h"
#include "clutter/clutter-private.h"

struct _ClutterDesaturateEffect
{
  ClutterOffscreenEffect parent_instance;

  /* the desaturation factor, also known as "strength" */
  gdouble factor;

  gint factor_uniform;

  gint tex_width;
  gint tex_height;

  CoglPipeline *pipeline;
};

struct _ClutterDesaturateEffectClass
{
  ClutterOffscreenEffectClass parent_class;

  CoglPipeline *base_pipeline;
};

/* the magic gray vec3 has been taken from the NTSC conversion weights
 * as defined by:
 *
 *   "OpenGL Superbible, 4th edition"
 *   -- Richard S. Wright Jr, Benjamin Lipchak, Nicholas Haemel
 *   Addison-Wesley
 */
static const gchar *desaturate_glsl_declarations =
  "uniform float factor;\n"
  "\n"
  "vec3 desaturate (const vec3 color, const float desaturation)\n"
  "{\n"
  "  const vec3 gray_conv = vec3 (0.299, 0.587, 0.114);\n"
  "  vec3 gray = vec3 (dot (gray_conv, color));\n"
  "  return vec3 (mix (color.rgb, gray, desaturation));\n"
  "}\n";

static const gchar *desaturate_glsl_source =
  "  cogl_color_out.rgb = desaturate (cogl_color_out.rgb, factor);\n";

enum
{
  PROP_0,

  PROP_FACTOR,

  PROP_LAST
};

static GParamSpec *obj_props[PROP_LAST];

G_DEFINE_TYPE (ClutterDesaturateEffect,
               clutter_desaturate_effect,
               CLUTTER_TYPE_OFFSCREEN_EFFECT);

static CoglPipeline *
clutter_desaturate_effect_create_pipeline (ClutterOffscreenEffect *effect,
                                           CoglTexture            *texture)
{
  ClutterDesaturateEffect *desaturate_effect =
    CLUTTER_DESATURATE_EFFECT (effect);

  cogl_pipeline_set_layer_texture (desaturate_effect->pipeline, 0, texture);

  return cogl_object_ref (desaturate_effect->pipeline);
}

static void
clutter_desaturate_effect_dispose (GObject *gobject)
{
  ClutterDesaturateEffect *self = CLUTTER_DESATURATE_EFFECT (gobject);

  if (self->pipeline != NULL)
    {
      cogl_object_unref (self->pipeline);
      self->pipeline = NULL;
    }

  G_OBJECT_CLASS (clutter_desaturate_effect_parent_class)->dispose (gobject);
}

static void
clutter_desaturate_effect_set_property (GObject      *gobject,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  ClutterDesaturateEffect *effect = CLUTTER_DESATURATE_EFFECT (gobject);

  switch (prop_id)
    {
    case PROP_FACTOR:
      clutter_desaturate_effect_set_factor (effect,
                                            g_value_get_double (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
clutter_desaturate_effect_get_property (GObject    *gobject,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  ClutterDesaturateEffect *effect = CLUTTER_DESATURATE_EFFECT (gobject);

  switch (prop_id)
    {
    case PROP_FACTOR:
      g_value_set_double (value, effect->factor);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
update_factor_uniform (ClutterDesaturateEffect *self)
{
  if (self->factor_uniform > -1)
    cogl_pipeline_set_uniform_1f (self->pipeline,
                                  self->factor_uniform,
                                  self->factor);
}

static void
clutter_desaturate_effect_class_init (ClutterDesaturateEffectClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  ClutterOffscreenEffectClass *offscreen_class;

  offscreen_class = CLUTTER_OFFSCREEN_EFFECT_CLASS (klass);
  offscreen_class->create_pipeline = clutter_desaturate_effect_create_pipeline;

  /**
   * ClutterDesaturateEffect:factor:
   *
   * The desaturation factor, between 0.0 (no desaturation) and 1.0 (full
   * desaturation).
   */
  obj_props[PROP_FACTOR] =
    g_param_spec_double ("factor", NULL, NULL,
                         0.0, 1.0,
                         1.0,
                         CLUTTER_PARAM_READWRITE);

  gobject_class->dispose = clutter_desaturate_effect_dispose;
  gobject_class->set_property = clutter_desaturate_effect_set_property;
  gobject_class->get_property = clutter_desaturate_effect_get_property;

  g_object_class_install_properties (gobject_class, PROP_LAST, obj_props);
}

static void
clutter_desaturate_effect_init (ClutterDesaturateEffect *self)
{
  ClutterDesaturateEffectClass *klass = CLUTTER_DESATURATE_EFFECT_GET_CLASS (self);

  if (G_UNLIKELY (klass->base_pipeline == NULL))
    {
      CoglContext *ctx =
        clutter_backend_get_cogl_context (clutter_get_default_backend ());
      CoglSnippet *snippet;

      klass->base_pipeline = cogl_pipeline_new (ctx);

      snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
                                  desaturate_glsl_declarations,
                                  desaturate_glsl_source);
      cogl_pipeline_add_snippet (klass->base_pipeline, snippet);
      cogl_object_unref (snippet);

      cogl_pipeline_set_layer_null_texture (klass->base_pipeline, 0);
    }

  self->pipeline = cogl_pipeline_copy (klass->base_pipeline);

  self->factor_uniform =
    cogl_pipeline_get_uniform_location (self->pipeline, "factor");

  self->factor = 1.0;

  update_factor_uniform (self);
}

/**
 * clutter_desaturate_effect_new:
 * @factor: the desaturation factor, between 0.0 and 1.0
 *
 * Creates a new #ClutterDesaturateEffect to be used with
 * clutter_actor_add_effect()
 *
 * Return value: the newly created #ClutterDesaturateEffect or %NULL
 */
ClutterEffect *
clutter_desaturate_effect_new (gdouble factor)
{
  g_return_val_if_fail (factor >= 0.0 && factor <= 1.0, NULL);

  return g_object_new (CLUTTER_TYPE_DESATURATE_EFFECT,
                       "factor", factor,
                       NULL);
}

/**
 * clutter_desaturate_effect_set_factor:
 * @effect: a #ClutterDesaturateEffect
 * @factor: the desaturation factor, between 0.0 and 1.0
 *
 * Sets the desaturation factor for @effect, with 0.0 being "do not desaturate"
 * and 1.0 being "fully desaturate"
 */
void
clutter_desaturate_effect_set_factor (ClutterDesaturateEffect *effect,
                                      double                   factor)
{
  g_return_if_fail (CLUTTER_IS_DESATURATE_EFFECT (effect));
  g_return_if_fail (factor >= 0.0 && factor <= 1.0);

  if (fabs (effect->factor - factor) >= 0.00001)
    {
      effect->factor = factor;
      update_factor_uniform (effect);

      clutter_effect_queue_repaint (CLUTTER_EFFECT (effect));

      g_object_notify_by_pspec (G_OBJECT (effect), obj_props[PROP_FACTOR]);
    }
}

/**
 * clutter_desaturate_effect_get_factor:
 * @effect: a #ClutterDesaturateEffect
 *
 * Retrieves the desaturation factor of @effect
 *
 * Return value: the desaturation factor
 */
gdouble
clutter_desaturate_effect_get_factor (ClutterDesaturateEffect *effect)
{
  g_return_val_if_fail (CLUTTER_IS_DESATURATE_EFFECT (effect), 0.0);

  return effect->factor;
}
