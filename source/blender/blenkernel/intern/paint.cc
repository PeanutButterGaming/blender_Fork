/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cstdlib>
#include <cstring>
#include <optional>

#include "DNA_object_enums.h"
#include "MEM_guardedalloc.h"

#include "DNA_asset_types.h"
#include "DNA_brush_types.h"
#include "DNA_defaults.h"
#include "DNA_gpencil_legacy_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "BLI_bitmap.h"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BKE_asset.hh"
#include "BKE_asset_edit.hh"
#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_deform.hh"
#include "BKE_gpencil_legacy.h"
#include "BKE_idtype.hh"
#include "BKE_image.h"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"
#include "BKE_scene.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subsurf.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_enum_types.hh"

#include "BLO_read_write.hh"

#include "bmesh.hh"

using blender::float3;
using blender::MutableSpan;
using blender::Span;
using blender::Vector;
using blender::bke::AttrDomain;

static void sculpt_attribute_update_refs(Object *ob, blender::bke::pbvh::Type pbvhtype);
static SculptAttribute *sculpt_attribute_ensure_ex(Object *ob,
                                                   AttrDomain domain,
                                                   eCustomDataType proptype,
                                                   const char *name,
                                                   const SculptAttributeParams *params,
                                                   blender::bke::pbvh::Type pbvhtype,
                                                   bool flat_array_for_bmesh);

static void palette_init_data(ID *id)
{
  Palette *palette = (Palette *)id;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(palette, id));

  /* Enable fake user by default. */
  id_fake_user_set(&palette->id);
}

static void palette_copy_data(Main * /*bmain*/,
                              std::optional<Library *> /*owner_library*/,
                              ID *id_dst,
                              const ID *id_src,
                              const int /*flag*/)
{
  Palette *palette_dst = (Palette *)id_dst;
  const Palette *palette_src = (const Palette *)id_src;

  BLI_duplicatelist(&palette_dst->colors, &palette_src->colors);
}

static void palette_free_data(ID *id)
{
  Palette *palette = (Palette *)id;

  BLI_freelistN(&palette->colors);
}

static void palette_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Palette *palette = (Palette *)id;

  BLO_write_id_struct(writer, Palette, id_address, &palette->id);
  BKE_id_blend_write(writer, &palette->id);

  BLO_write_struct_list(writer, PaletteColor, &palette->colors);
}

static void palette_blend_read_data(BlendDataReader *reader, ID *id)
{
  Palette *palette = (Palette *)id;
  BLO_read_struct_list(reader, PaletteColor, &palette->colors);
}

static void palette_undo_preserve(BlendLibReader * /*reader*/, ID *id_new, ID *id_old)
{
  /* Whole Palette is preserved across undo-steps, and it has no extra pointer, simple. */
  /* NOTE: We do not care about potential internal references to self here, Palette has none. */
  /* NOTE: We do not swap IDProperties, as dealing with potential ID pointers in those would be
   *       fairly delicate. */
  BKE_lib_id_swap(nullptr, id_new, id_old, false, 0);
  std::swap(id_new->properties, id_old->properties);
}

IDTypeInfo IDType_ID_PAL = {
    /*id_code*/ ID_PAL,
    /*id_filter*/ FILTER_ID_PAL,
    /*dependencies_id_types*/ 0,
    /*main_listbase_index*/ INDEX_ID_PAL,
    /*struct_size*/ sizeof(Palette),
    /*name*/ "Palette",
    /*name_plural*/ N_("palettes"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_PALETTE,
    /*flags*/ IDTYPE_FLAGS_NO_ANIMDATA,
    /*asset_type_info*/ nullptr,

    /*init_data*/ palette_init_data,
    /*copy_data*/ palette_copy_data,
    /*free_data*/ palette_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ palette_blend_write,
    /*blend_read_data*/ palette_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ palette_undo_preserve,

    /*lib_override_apply_post*/ nullptr,
};

static void paint_curve_copy_data(Main * /*bmain*/,
                                  std::optional<Library *> /*owner_library*/,
                                  ID *id_dst,
                                  const ID *id_src,
                                  const int /*flag*/)
{
  PaintCurve *paint_curve_dst = (PaintCurve *)id_dst;
  const PaintCurve *paint_curve_src = (const PaintCurve *)id_src;

  if (paint_curve_src->tot_points != 0) {
    paint_curve_dst->points = static_cast<PaintCurvePoint *>(
        MEM_dupallocN(paint_curve_src->points));
  }
}

static void paint_curve_free_data(ID *id)
{
  PaintCurve *paint_curve = (PaintCurve *)id;

  MEM_SAFE_FREE(paint_curve->points);
  paint_curve->tot_points = 0;
}

static void paint_curve_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  PaintCurve *pc = (PaintCurve *)id;

  BLO_write_id_struct(writer, PaintCurve, id_address, &pc->id);
  BKE_id_blend_write(writer, &pc->id);

  BLO_write_struct_array(writer, PaintCurvePoint, pc->tot_points, pc->points);
}

static void paint_curve_blend_read_data(BlendDataReader *reader, ID *id)
{
  PaintCurve *pc = (PaintCurve *)id;
  BLO_read_struct_array(reader, PaintCurvePoint, pc->tot_points, &pc->points);
}

IDTypeInfo IDType_ID_PC = {
    /*id_code*/ ID_PC,
    /*id_filter*/ FILTER_ID_PC,
    /*dependencies_id_types*/ 0,
    /*main_listbase_index*/ INDEX_ID_PC,
    /*struct_size*/ sizeof(PaintCurve),
    /*name*/ "PaintCurve",
    /*name_plural*/ N_("paint_curves"),
    /*translation_context*/ BLT_I18NCONTEXT_ID_PAINTCURVE,
    /*flags*/ IDTYPE_FLAGS_NO_ANIMDATA,
    /*asset_type_info*/ nullptr,

    /*init_data*/ nullptr,
    /*copy_data*/ paint_curve_copy_data,
    /*free_data*/ paint_curve_free_data,
    /*make_local*/ nullptr,
    /*foreach_id*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_path*/ nullptr,
    /*owner_pointer_get*/ nullptr,

    /*blend_write*/ paint_curve_blend_write,
    /*blend_read_data*/ paint_curve_blend_read_data,
    /*blend_read_after_liblink*/ nullptr,

    /*blend_read_undo_preserve*/ nullptr,

    /*lib_override_apply_post*/ nullptr,
};

const uchar PAINT_CURSOR_SCULPT[3] = {255, 100, 100};
const uchar PAINT_CURSOR_VERTEX_PAINT[3] = {255, 255, 255};
const uchar PAINT_CURSOR_WEIGHT_PAINT[3] = {200, 200, 255};
const uchar PAINT_CURSOR_TEXTURE_PAINT[3] = {255, 255, 255};
const uchar PAINT_CURSOR_SCULPT_CURVES[3] = {255, 100, 100};
const uchar PAINT_CURSOR_PAINT_GREASE_PENCIL[3] = {255, 100, 100};
const uchar PAINT_CURSOR_SCULPT_GREASE_PENCIL[3] = {255, 100, 100};

static ePaintOverlayControlFlags overlay_flags = (ePaintOverlayControlFlags)0;

void BKE_paint_invalidate_overlay_tex(Scene *scene, ViewLayer *view_layer, const Tex *tex)
{
  Paint *paint = BKE_paint_get_active(scene, view_layer);
  if (!paint) {
    return;
  }

  Brush *br = BKE_paint_brush(paint);
  if (!br) {
    return;
  }

  if (br->mtex.tex == tex) {
    overlay_flags |= PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY;
  }
  if (br->mask_mtex.tex == tex) {
    overlay_flags |= PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY;
  }
}

void BKE_paint_invalidate_cursor_overlay(Scene *scene, ViewLayer *view_layer, CurveMapping *curve)
{
  Paint *paint = BKE_paint_get_active(scene, view_layer);
  if (paint == nullptr) {
    return;
  }

  Brush *br = BKE_paint_brush(paint);
  if (br && br->curve == curve) {
    overlay_flags |= PAINT_OVERLAY_INVALID_CURVE;
  }
}

void BKE_paint_invalidate_overlay_all()
{
  overlay_flags |= (PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY |
                    PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY | PAINT_OVERLAY_INVALID_CURVE);
}

ePaintOverlayControlFlags BKE_paint_get_overlay_flags()
{
  return overlay_flags;
}

void BKE_paint_set_overlay_override(eOverlayFlags flags)
{
  if (flags & BRUSH_OVERLAY_OVERRIDE_MASK) {
    if (flags & BRUSH_OVERLAY_CURSOR_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_CURSOR;
    }
    if (flags & BRUSH_OVERLAY_PRIMARY_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_PRIMARY;
    }
    if (flags & BRUSH_OVERLAY_SECONDARY_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_SECONDARY;
    }
  }
  else {
    overlay_flags &= ~(PAINT_OVERRIDE_MASK);
  }
}

void BKE_paint_reset_overlay_invalid(ePaintOverlayControlFlags flag)
{
  overlay_flags &= ~(flag);
}

bool BKE_paint_ensure_from_paintmode(Main *bmain, Scene *sce, PaintMode mode)
{
  ToolSettings *ts = sce->toolsettings;
  Paint **paint_ptr = nullptr;
  /* Some paint modes don't store paint settings as pointer, for these this can be set and
   * referenced by paint_ptr. */
  Paint *paint_tmp = nullptr;

  switch (mode) {
    case PaintMode::Sculpt:
      paint_ptr = (Paint **)&ts->sculpt;
      break;
    case PaintMode::Vertex:
      paint_ptr = (Paint **)&ts->vpaint;
      break;
    case PaintMode::Weight:
      paint_ptr = (Paint **)&ts->wpaint;
      break;
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      paint_tmp = (Paint *)&ts->imapaint;
      paint_ptr = &paint_tmp;
      break;
    case PaintMode::GPencil:
      paint_ptr = (Paint **)&ts->gp_paint;
      break;
    case PaintMode::VertexGPencil:
      paint_ptr = (Paint **)&ts->gp_vertexpaint;
      break;
    case PaintMode::SculptGPencil:
      paint_ptr = (Paint **)&ts->gp_sculptpaint;
      break;
    case PaintMode::WeightGPencil:
      paint_ptr = (Paint **)&ts->gp_weightpaint;
      break;
    case PaintMode::SculptCurves:
      paint_ptr = (Paint **)&ts->curves_sculpt;
      break;
    case PaintMode::SculptGreasePencil:
      paint_ptr = (Paint **)&ts->gp_sculptpaint;
      break;
    case PaintMode::Invalid:
      break;
  }
  if (paint_ptr) {
    BKE_paint_ensure(bmain, ts, paint_ptr);
    return true;
  }
  return false;
}

Paint *BKE_paint_get_active_from_paintmode(Scene *sce, PaintMode mode)
{
  if (sce) {
    ToolSettings *ts = sce->toolsettings;

    switch (mode) {
      case PaintMode::Sculpt:
        return &ts->sculpt->paint;
      case PaintMode::Vertex:
        return &ts->vpaint->paint;
      case PaintMode::Weight:
        return &ts->wpaint->paint;
      case PaintMode::Texture2D:
      case PaintMode::Texture3D:
        return &ts->imapaint.paint;
      case PaintMode::GPencil:
        return &ts->gp_paint->paint;
      case PaintMode::VertexGPencil:
        return &ts->gp_vertexpaint->paint;
      case PaintMode::SculptGPencil:
        return &ts->gp_sculptpaint->paint;
      case PaintMode::WeightGPencil:
        return &ts->gp_weightpaint->paint;
      case PaintMode::SculptCurves:
        return &ts->curves_sculpt->paint;
      case PaintMode::SculptGreasePencil:
        return &ts->gp_sculptpaint->paint;
      case PaintMode::Invalid:
        return nullptr;
      default:
        return &ts->imapaint.paint;
    }
  }

  return nullptr;
}

const EnumPropertyItem *BKE_paint_get_tool_enum_from_paintmode(const PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
      return rna_enum_brush_sculpt_brush_type_items;
    case PaintMode::Vertex:
      return rna_enum_brush_vertex_brush_type_items;
    case PaintMode::Weight:
      return rna_enum_brush_weight_brush_type_items;
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      return rna_enum_brush_image_brush_type_items;
    case PaintMode::GPencil:
      return rna_enum_brush_gpencil_types_items;
    case PaintMode::VertexGPencil:
      return rna_enum_brush_gpencil_vertex_types_items;
    case PaintMode::SculptGPencil:
      return rna_enum_brush_gpencil_sculpt_types_items;
    case PaintMode::WeightGPencil:
      return rna_enum_brush_gpencil_weight_types_items;
    case PaintMode::SculptCurves:
      return rna_enum_brush_curves_sculpt_brush_type_items;
    case PaintMode::SculptGreasePencil:
      return rna_enum_brush_gpencil_sculpt_types_items;
    case PaintMode::Invalid:
      break;
  }
  return nullptr;
}

Paint *BKE_paint_get_active(Scene *sce, ViewLayer *view_layer)
{
  if (sce && view_layer) {
    ToolSettings *ts = sce->toolsettings;
    BKE_view_layer_synced_ensure(sce, view_layer);
    Object *actob = BKE_view_layer_active_object_get(view_layer);

    if (actob) {
      switch (actob->mode) {
        case OB_MODE_SCULPT:
          return &ts->sculpt->paint;
        case OB_MODE_VERTEX_PAINT:
          return &ts->vpaint->paint;
        case OB_MODE_WEIGHT_PAINT:
          return &ts->wpaint->paint;
        case OB_MODE_TEXTURE_PAINT:
          return &ts->imapaint.paint;
        case OB_MODE_PAINT_GPENCIL_LEGACY:
          return &ts->gp_paint->paint;
        case OB_MODE_VERTEX_GPENCIL_LEGACY:
          return &ts->gp_vertexpaint->paint;
        case OB_MODE_SCULPT_GPENCIL_LEGACY:
          return &ts->gp_sculptpaint->paint;
        case OB_MODE_WEIGHT_GPENCIL_LEGACY:
          return &ts->gp_weightpaint->paint;
        case OB_MODE_SCULPT_CURVES:
          return &ts->curves_sculpt->paint;
        default:
          break;
      }
    }

    /* default to image paint */
    return &ts->imapaint.paint;
  }

  return nullptr;
}

Paint *BKE_paint_get_active_from_context(const bContext *C)
{
  Scene *sce = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  SpaceImage *sima;

  if (sce && view_layer) {
    ToolSettings *ts = sce->toolsettings;
    BKE_view_layer_synced_ensure(sce, view_layer);
    Object *obact = BKE_view_layer_active_object_get(view_layer);

    if ((sima = CTX_wm_space_image(C)) != nullptr) {
      if (obact && obact->mode == OB_MODE_EDIT) {
        if (sima->mode == SI_MODE_PAINT) {
          return &ts->imapaint.paint;
        }
      }
      else {
        return &ts->imapaint.paint;
      }
    }
    else {
      return BKE_paint_get_active(sce, view_layer);
    }
  }

  return nullptr;
}

PaintMode BKE_paintmode_get_active_from_context(const bContext *C)
{
  Scene *sce = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  SpaceImage *sima;

  if (sce && view_layer) {
    BKE_view_layer_synced_ensure(sce, view_layer);
    Object *obact = BKE_view_layer_active_object_get(view_layer);

    if ((sima = CTX_wm_space_image(C)) != nullptr) {
      if (obact && obact->mode == OB_MODE_EDIT) {
        if (sima->mode == SI_MODE_PAINT) {
          return PaintMode::Texture2D;
        }
      }
      else {
        return PaintMode::Texture2D;
      }
    }
    else if (obact) {
      switch (obact->mode) {
        case OB_MODE_SCULPT:
          return PaintMode::Sculpt;
        case OB_MODE_SCULPT_GPENCIL_LEGACY:
          if (obact->type == OB_GPENCIL_LEGACY) {
            return PaintMode::SculptGPencil;
          }
          if (obact->type == OB_GREASE_PENCIL) {
            return PaintMode::SculptGreasePencil;
          }
          return PaintMode::Invalid;
        case OB_MODE_PAINT_GPENCIL_LEGACY:
          return PaintMode::GPencil;
        case OB_MODE_WEIGHT_GPENCIL_LEGACY:
          return PaintMode::WeightGPencil;
        case OB_MODE_VERTEX_PAINT:
          return PaintMode::Vertex;
        case OB_MODE_WEIGHT_PAINT:
          return PaintMode::Weight;
        case OB_MODE_TEXTURE_PAINT:
          return PaintMode::Texture3D;
        case OB_MODE_SCULPT_CURVES:
          return PaintMode::SculptCurves;
        default:
          return PaintMode::Texture2D;
      }
    }
    else {
      /* default to image paint */
      return PaintMode::Texture2D;
    }
  }

  return PaintMode::Invalid;
}

PaintMode BKE_paintmode_get_from_tool(const bToolRef *tref)
{
  if (tref->space_type == SPACE_VIEW3D) {
    switch (tref->mode) {
      case CTX_MODE_SCULPT:
        return PaintMode::Sculpt;
      case CTX_MODE_PAINT_VERTEX:
        return PaintMode::Vertex;
      case CTX_MODE_PAINT_WEIGHT:
        return PaintMode::Weight;
      case CTX_MODE_PAINT_GPENCIL_LEGACY:
        return PaintMode::GPencil;
      case CTX_MODE_PAINT_TEXTURE:
        return PaintMode::Texture3D;
      case CTX_MODE_VERTEX_GPENCIL_LEGACY:
        return PaintMode::VertexGPencil;
      case CTX_MODE_SCULPT_GPENCIL_LEGACY:
        return PaintMode::SculptGPencil;
      case CTX_MODE_WEIGHT_GREASE_PENCIL:
      case CTX_MODE_WEIGHT_GPENCIL_LEGACY:
        return PaintMode::WeightGPencil;
      case CTX_MODE_SCULPT_CURVES:
        return PaintMode::SculptCurves;
      case CTX_MODE_PAINT_GREASE_PENCIL:
        return PaintMode::GPencil;
      case CTX_MODE_SCULPT_GREASE_PENCIL:
        return PaintMode::SculptGreasePencil;
    }
  }
  else if (tref->space_type == SPACE_IMAGE) {
    switch (tref->mode) {
      case SI_MODE_PAINT:
        return PaintMode::Texture2D;
    }
  }

  return PaintMode::Invalid;
}

static bool paint_brush_set_from_asset_reference(Main *bmain, Paint *paint)
{
  /* Don't resolve this during file read, it will be done after. */
  if (bmain->is_locked_for_linking) {
    return false;
  }
  /* Attempt to restore a valid active brush from brush asset information. */
  if (paint->brush != nullptr) {
    return false;
  }
  if (paint->brush_asset_reference == nullptr) {
    return false;
  }

  Brush *brush = reinterpret_cast<Brush *>(blender::bke::asset_edit_id_from_weak_reference(
      *bmain, ID_BR, *paint->brush_asset_reference));
  BLI_assert(brush == nullptr || blender::bke::asset_edit_id_is_editable(brush->id));

  /* Ensure we have a brush with appropriate mode to assign.
   * Could happen if contents of asset blend was manually changed. */
  if (brush == nullptr || (paint->runtime.ob_mode & brush->ob_mode) == 0) {
    MEM_delete(paint->brush_asset_reference);
    paint->brush_asset_reference = nullptr;
    return false;
  }

  paint->brush = brush;
  return true;
}

Brush *BKE_paint_brush(Paint *paint)
{
  return (Brush *)BKE_paint_brush_for_read((const Paint *)paint);
}

const Brush *BKE_paint_brush_for_read(const Paint *paint)
{
  return paint ? paint->brush : nullptr;
}

bool BKE_paint_brush_set(Paint *paint, Brush *brush)
{
  if (paint == nullptr) {
    return false;
  }
  if (brush && (paint->runtime.ob_mode & brush->ob_mode) == 0) {
    return false;
  }

  paint->brush = brush;

  MEM_delete(paint->brush_asset_reference);
  paint->brush_asset_reference = nullptr;

  if (brush != nullptr) {
    std::optional<AssetWeakReference> weak_ref = blender::bke::asset_edit_weak_reference_from_id(
        brush->id);
    if (weak_ref.has_value()) {
      paint->brush_asset_reference = MEM_new<AssetWeakReference>(__func__, *weak_ref);
    }
  }

  return true;
}

Brush *BKE_paint_brush_from_essentials(Main *bmain, const char *name)
{
  AssetWeakReference weak_ref;
  weak_ref.asset_library_type = eAssetLibraryType::ASSET_LIBRARY_ESSENTIALS;
  weak_ref.relative_asset_identifier = BLI_sprintfN("brushes/essentials_brushes.blend/Brush/%s",
                                                    name);

  return reinterpret_cast<Brush *>(
      blender::bke::asset_edit_id_from_weak_reference(*bmain, ID_BR, weak_ref));
}

static void paint_brush_set_essentials_reference(Paint *paint, const char *name)
{
  /* Set brush asset reference to a named brush in the essentials asset library. */
  MEM_delete(paint->brush_asset_reference);

  AssetWeakReference *weak_ref = MEM_new<AssetWeakReference>(__func__);
  weak_ref->asset_library_type = eAssetLibraryType::ASSET_LIBRARY_ESSENTIALS;
  weak_ref->relative_asset_identifier = BLI_sprintfN("brushes/essentials_brushes.blend/Brush/%s",
                                                     name);
  paint->brush_asset_reference = weak_ref;
  paint->brush = nullptr;
}

static void paint_eraser_brush_set_essentials_reference(Paint *paint, const char *name)
{
  /* Set brush asset reference to a named brush in the essentials asset library. */
  MEM_delete(paint->eraser_brush_asset_reference);

  AssetWeakReference *weak_ref = MEM_new<AssetWeakReference>(__func__);
  weak_ref->asset_library_type = eAssetLibraryType::ASSET_LIBRARY_ESSENTIALS;
  weak_ref->relative_asset_identifier = BLI_sprintfN("brushes/essentials_brushes.blend/Brush/%s",
                                                     name);
  paint->eraser_brush_asset_reference = weak_ref;
  paint->eraser_brush = nullptr;
}

static void paint_brush_set_default_reference(Paint *paint,
                                              const bool do_regular = true,
                                              const bool do_eraser = true)
{
  if (!paint->runtime.initialized) {
    /* Can happen when loading old file where toolsettings are created in versioning, without
     * calling #paint_runtime_init(). Will be done later when necessary. */
    return;
  }

  const char *name = nullptr;
  const char *eraser_name = nullptr;

  switch (paint->runtime.ob_mode) {
    case OB_MODE_SCULPT:
      name = "Draw";
      break;
    case OB_MODE_VERTEX_PAINT:
      name = "Paint Vertex";
      break;
    case OB_MODE_WEIGHT_PAINT:
      name = "Paint Weight";
      break;
    case OB_MODE_TEXTURE_PAINT:
      name = "Paint Texture";
      break;
    case OB_MODE_SCULPT_CURVES:
      name = "Comb Curves";
      break;
    case OB_MODE_PAINT_GPENCIL_LEGACY:
      name = "Pencil";
      eraser_name = "Eraser Soft";
      break;
    case OB_MODE_VERTEX_GPENCIL_LEGACY:
      name = "Paint Point Color";
      break;
    case OB_MODE_SCULPT_GPENCIL_LEGACY:
      name = "Smooth Stroke";
      break;
    case OB_MODE_WEIGHT_GPENCIL_LEGACY:
      name = "Paint Point Weight";
      break;
    default:
      BLI_assert_unreachable();
      return;
  }

  if (do_regular && name) {
    paint_brush_set_essentials_reference(paint, name);
  }
  if (do_eraser && eraser_name) {
    paint_eraser_brush_set_essentials_reference(paint, eraser_name);
  }
}

void BKE_paint_brushes_set_default_references(ToolSettings *ts)
{
  if (ts->sculpt) {
    paint_brush_set_default_reference(&ts->sculpt->paint);
  }
  if (ts->curves_sculpt) {
    paint_brush_set_default_reference(&ts->curves_sculpt->paint);
  }
  if (ts->wpaint) {
    paint_brush_set_default_reference(&ts->wpaint->paint);
  }
  if (ts->vpaint) {
    paint_brush_set_default_reference(&ts->vpaint->paint);
  }
  if (ts->gp_paint) {
    paint_brush_set_default_reference(&ts->gp_paint->paint);
  }
  if (ts->gp_vertexpaint) {
    paint_brush_set_default_reference(&ts->gp_vertexpaint->paint);
  }
  if (ts->gp_sculptpaint) {
    paint_brush_set_default_reference(&ts->gp_sculptpaint->paint);
  }
  if (ts->gp_weightpaint) {
    paint_brush_set_default_reference(&ts->gp_weightpaint->paint);
  }
  paint_brush_set_default_reference(&ts->imapaint.paint);
}

bool BKE_paint_brush_set_default(Main *bmain, Paint *paint)
{
  paint_brush_set_default_reference(paint, true, false);
  return paint_brush_set_from_asset_reference(bmain, paint);
}

bool BKE_paint_brush_set_essentials(Main *bmain, Paint *paint, const char *name)
{
  paint_brush_set_essentials_reference(paint, name);
  return paint_brush_set_from_asset_reference(bmain, paint);
}

void BKE_paint_brushes_validate(Main *bmain, Paint *paint)
{
  /* Clear brush with invalid mode. Unclear if this can still happen,
   * but kept from old paint tool-slots code. */
  Brush *brush = BKE_paint_brush(paint);
  if (brush && (paint->runtime.ob_mode & brush->ob_mode) == 0) {
    BKE_paint_brush_set(paint, nullptr);
    BKE_paint_brush_set_default(bmain, paint);
  }

  Brush *eraser_brush = BKE_paint_eraser_brush(paint);
  if (eraser_brush && (paint->runtime.ob_mode & eraser_brush->ob_mode) == 0) {
    BKE_paint_eraser_brush_set(paint, nullptr);
    BKE_paint_eraser_brush_set_default(bmain, paint);
  }
}

static bool paint_eraser_brush_set_from_asset_reference(Main *bmain, Paint *paint)
{
  /* Don't resolve this during file read, it will be done after. */
  if (bmain->is_locked_for_linking) {
    return false;
  }
  /* Attempt to restore a valid active brush from brush asset information. */
  if (paint->eraser_brush != nullptr) {
    return false;
  }
  if (paint->eraser_brush_asset_reference == nullptr) {
    return false;
  }

  Brush *brush = reinterpret_cast<Brush *>(blender::bke::asset_edit_id_from_weak_reference(
      *bmain, ID_BR, *paint->eraser_brush_asset_reference));
  BLI_assert(brush == nullptr || blender::bke::asset_edit_id_is_editable(brush->id));

  /* Ensure we have a brush with appropriate mode to assign.
   * Could happen if contents of asset blend was manually changed. */
  if (brush == nullptr || (paint->runtime.ob_mode & brush->ob_mode) == 0) {
    MEM_delete(paint->eraser_brush_asset_reference);
    paint->eraser_brush_asset_reference = nullptr;
    return false;
  }

  paint->eraser_brush = brush;
  return true;
}

Brush *BKE_paint_eraser_brush(Paint *paint)
{
  return (Brush *)BKE_paint_eraser_brush_for_read((const Paint *)paint);
}

const Brush *BKE_paint_eraser_brush_for_read(const Paint *paint)
{
  return paint ? paint->eraser_brush : nullptr;
}

bool BKE_paint_eraser_brush_set(Paint *paint, Brush *brush)
{
  if (paint == nullptr || paint->eraser_brush == brush) {
    return false;
  }
  if (brush && (paint->runtime.ob_mode & brush->ob_mode) == 0) {
    return false;
  }

  paint->eraser_brush = brush;

  MEM_delete(paint->eraser_brush_asset_reference);
  paint->eraser_brush_asset_reference = nullptr;

  if (brush != nullptr) {
    std::optional<AssetWeakReference> weak_ref = blender::bke::asset_edit_weak_reference_from_id(
        brush->id);
    if (weak_ref.has_value()) {
      paint->eraser_brush_asset_reference = MEM_new<AssetWeakReference>(__func__, *weak_ref);
    }
  }

  return true;
}

Brush *BKE_paint_eraser_brush_from_essentials(Main *bmain, const char *name)
{
  AssetWeakReference weak_ref;
  weak_ref.asset_library_type = eAssetLibraryType::ASSET_LIBRARY_ESSENTIALS;
  weak_ref.relative_asset_identifier = BLI_sprintfN("brushes/essentials_brushes.blend/Brush/%s",
                                                    name);

  return reinterpret_cast<Brush *>(
      blender::bke::asset_edit_id_from_weak_reference(*bmain, ID_BR, weak_ref));
}

bool BKE_paint_eraser_brush_set_default(Main *bmain, Paint *paint)
{
  paint_brush_set_default_reference(paint, false, true);
  return paint_eraser_brush_set_from_asset_reference(bmain, paint);
}

bool BKE_paint_eraser_brush_set_essentials(Main *bmain, Paint *paint, const char *name)
{
  paint_eraser_brush_set_essentials_reference(paint, name);
  return paint_eraser_brush_set_from_asset_reference(bmain, paint);
}

static void paint_runtime_init(const ToolSettings *ts, Paint *paint)
{
  if (paint == &ts->imapaint.paint) {
    paint->runtime.ob_mode = OB_MODE_TEXTURE_PAINT;
  }
  else if (ts->sculpt && paint == &ts->sculpt->paint) {
    paint->runtime.ob_mode = OB_MODE_SCULPT;
  }
  else if (ts->vpaint && paint == &ts->vpaint->paint) {
    paint->runtime.ob_mode = OB_MODE_VERTEX_PAINT;
  }
  else if (ts->wpaint && paint == &ts->wpaint->paint) {
    paint->runtime.ob_mode = OB_MODE_WEIGHT_PAINT;
  }
  else if (ts->gp_paint && paint == &ts->gp_paint->paint) {
    paint->runtime.ob_mode = OB_MODE_PAINT_GPENCIL_LEGACY;
  }
  else if (ts->gp_vertexpaint && paint == &ts->gp_vertexpaint->paint) {
    paint->runtime.ob_mode = OB_MODE_VERTEX_GPENCIL_LEGACY;
  }
  else if (ts->gp_sculptpaint && paint == &ts->gp_sculptpaint->paint) {
    paint->runtime.ob_mode = OB_MODE_SCULPT_GPENCIL_LEGACY;
  }
  else if (ts->gp_weightpaint && paint == &ts->gp_weightpaint->paint) {
    paint->runtime.ob_mode = OB_MODE_WEIGHT_GPENCIL_LEGACY;
  }
  else if (ts->curves_sculpt && paint == &ts->curves_sculpt->paint) {
    paint->runtime.ob_mode = OB_MODE_SCULPT_CURVES;
  }
  else {
    BLI_assert_unreachable();
  }

  paint->runtime.initialized = true;
}

uint BKE_paint_get_brush_type_offset_from_paintmode(const PaintMode mode)
{
  switch (mode) {
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      return offsetof(Brush, image_brush_type);
    case PaintMode::Sculpt:
      return offsetof(Brush, sculpt_brush_type);
    case PaintMode::Vertex:
      return offsetof(Brush, vertex_brush_type);
    case PaintMode::Weight:
      return offsetof(Brush, weight_brush_type);
    case PaintMode::GPencil:
      return offsetof(Brush, gpencil_brush_type);
    case PaintMode::VertexGPencil:
      return offsetof(Brush, gpencil_vertex_brush_type);
    case PaintMode::SculptGPencil:
      return offsetof(Brush, gpencil_sculpt_brush_type);
    case PaintMode::WeightGPencil:
      return offsetof(Brush, gpencil_weight_brush_type);
    case PaintMode::SculptCurves:
      return offsetof(Brush, curves_sculpt_brush_type);
    case PaintMode::SculptGreasePencil:
      return offsetof(Brush, gpencil_sculpt_brush_type);
    case PaintMode::Invalid:
      break; /* We don't use these yet. */
  }
  return 0;
}

std::optional<int> BKE_paint_get_brush_type_from_obmode(const Brush *brush,
                                                        const eObjectMode ob_mode)
{
  switch (ob_mode) {
    case OB_MODE_TEXTURE_PAINT:
    case OB_MODE_EDIT:
      return brush->image_brush_type;
    case OB_MODE_SCULPT:
      return brush->sculpt_brush_type;
    case OB_MODE_VERTEX_PAINT:
      return brush->vertex_brush_type;
    case OB_MODE_WEIGHT_PAINT:
      return brush->weight_brush_type;
    case OB_MODE_PAINT_GPENCIL_LEGACY:
      return brush->gpencil_brush_type;
    case OB_MODE_VERTEX_GPENCIL_LEGACY:
      return brush->gpencil_vertex_brush_type;
    case OB_MODE_SCULPT_GPENCIL_LEGACY:
      return brush->gpencil_sculpt_brush_type;
    case OB_MODE_WEIGHT_GPENCIL_LEGACY:
      return brush->gpencil_weight_brush_type;
    case OB_MODE_SCULPT_CURVES:
      return brush->curves_sculpt_brush_type;
    default:
      return {};
  }
}

PaintCurve *BKE_paint_curve_add(Main *bmain, const char *name)
{
  PaintCurve *pc = static_cast<PaintCurve *>(BKE_id_new(bmain, ID_PC, name));
  return pc;
}

Palette *BKE_paint_palette(Paint *paint)
{
  return paint ? paint->palette : nullptr;
}

void BKE_paint_palette_set(Paint *paint, Palette *palette)
{
  if (paint) {
    id_us_min((ID *)paint->palette);
    paint->palette = palette;
    id_us_plus((ID *)paint->palette);
  }
}

void BKE_paint_curve_clamp_endpoint_add_index(PaintCurve *pc, const int add_index)
{
  pc->add_index = (add_index || pc->tot_points == 1) ? (add_index + 1) : 0;
}

void BKE_palette_color_remove(Palette *palette, PaletteColor *color)
{
  if (BLI_listbase_count_at_most(&palette->colors, palette->active_color) == palette->active_color)
  {
    palette->active_color--;
  }

  BLI_remlink(&palette->colors, color);

  if (palette->active_color < 0 && !BLI_listbase_is_empty(&palette->colors)) {
    palette->active_color = 0;
  }

  MEM_freeN(color);
}

void BKE_palette_clear(Palette *palette)
{
  BLI_freelistN(&palette->colors);
  palette->active_color = 0;
}

Palette *BKE_palette_add(Main *bmain, const char *name)
{
  Palette *palette = static_cast<Palette *>(BKE_id_new(bmain, ID_PAL, name));
  return palette;
}

PaletteColor *BKE_palette_color_add(Palette *palette)
{
  PaletteColor *color = MEM_cnew<PaletteColor>(__func__);
  BLI_addtail(&palette->colors, color);
  return color;
}

bool BKE_palette_is_empty(const Palette *palette)
{
  return BLI_listbase_is_empty(&palette->colors);
}

/* helper function to sort using qsort */
static int palettecolor_compare_hsv(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = static_cast<const tPaletteColorHSV *>(a1);
  const tPaletteColorHSV *ps2 = static_cast<const tPaletteColorHSV *>(a2);

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  return 0;
}

/* helper function to sort using qsort */
static int palettecolor_compare_svh(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = static_cast<const tPaletteColorHSV *>(a1);
  const tPaletteColorHSV *ps2 = static_cast<const tPaletteColorHSV *>(a2);

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  return 0;
}

static int palettecolor_compare_vhs(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = static_cast<const tPaletteColorHSV *>(a1);
  const tPaletteColorHSV *ps2 = static_cast<const tPaletteColorHSV *>(a2);

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  return 0;
}

static int palettecolor_compare_luminance(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = static_cast<const tPaletteColorHSV *>(a1);
  const tPaletteColorHSV *ps2 = static_cast<const tPaletteColorHSV *>(a2);

  float lumi1 = (ps1->rgb[0] + ps1->rgb[1] + ps1->rgb[2]) / 3.0f;
  float lumi2 = (ps2->rgb[0] + ps2->rgb[1] + ps2->rgb[2]) / 3.0f;

  if (lumi1 > lumi2) {
    return -1;
  }
  if (lumi1 < lumi2) {
    return 1;
  }

  return 0;
}

void BKE_palette_sort_hsv(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Hue, Saturation and Value. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_hsv);
}

void BKE_palette_sort_svh(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Saturation, Value and Hue. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_svh);
}

void BKE_palette_sort_vhs(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Saturation, Value and Hue. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_vhs);
}

void BKE_palette_sort_luminance(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Luminance (calculated with the average, enough for sorting). */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_luminance);
}

bool BKE_palette_from_hash(Main *bmain, GHash *color_table, const char *name, const bool linear)
{
  tPaletteColorHSV *color_array = nullptr;
  tPaletteColorHSV *col_elm = nullptr;
  bool done = false;

  const int totpal = BLI_ghash_len(color_table);

  if (totpal > 0) {
    color_array = static_cast<tPaletteColorHSV *>(
        MEM_calloc_arrayN(totpal, sizeof(tPaletteColorHSV), __func__));
    /* Put all colors in an array. */
    GHashIterator gh_iter;
    int t = 0;
    GHASH_ITER (gh_iter, color_table) {
      const uint col = POINTER_AS_INT(BLI_ghashIterator_getValue(&gh_iter));
      float r, g, b;
      float h, s, v;
      cpack_to_rgb(col, &r, &g, &b);
      rgb_to_hsv(r, g, b, &h, &s, &v);

      col_elm = &color_array[t];
      col_elm->rgb[0] = r;
      col_elm->rgb[1] = g;
      col_elm->rgb[2] = b;
      col_elm->h = h;
      col_elm->s = s;
      col_elm->v = v;
      t++;
    }
  }

  /* Create the Palette. */
  if (totpal > 0) {
    /* Sort by Hue and saturation. */
    BKE_palette_sort_hsv(color_array, totpal);

    Palette *palette = BKE_palette_add(bmain, name);
    if (palette) {
      for (int i = 0; i < totpal; i++) {
        col_elm = &color_array[i];
        PaletteColor *palcol = BKE_palette_color_add(palette);
        if (palcol) {
          copy_v3_v3(palcol->rgb, col_elm->rgb);
          if (linear) {
            linearrgb_to_srgb_v3_v3(palcol->rgb, palcol->rgb);
          }
        }
      }
      done = true;
    }
  }
  else {
    done = false;
  }

  if (totpal > 0) {
    MEM_SAFE_FREE(color_array);
  }

  return done;
}

bool BKE_paint_select_face_test(const Object *ob)
{
  return ((ob != nullptr) && (ob->type == OB_MESH) && (ob->data != nullptr) &&
          (((Mesh *)ob->data)->editflag & ME_EDIT_PAINT_FACE_SEL) &&
          (ob->mode & (OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT | OB_MODE_TEXTURE_PAINT)));
}

bool BKE_paint_select_vert_test(const Object *ob)
{
  return ((ob != nullptr) && (ob->type == OB_MESH) && (ob->data != nullptr) &&
          (((Mesh *)ob->data)->editflag & ME_EDIT_PAINT_VERT_SEL) &&
          (ob->mode & OB_MODE_WEIGHT_PAINT || ob->mode & OB_MODE_VERTEX_PAINT));
}

bool BKE_paint_select_elem_test(const Object *ob)
{
  return (BKE_paint_select_vert_test(ob) || BKE_paint_select_face_test(ob));
}

bool BKE_paint_always_hide_test(const Object *ob)
{
  return ((ob != nullptr) && (ob->type == OB_MESH) && (ob->data != nullptr) &&
          (ob->mode & OB_MODE_WEIGHT_PAINT || ob->mode & OB_MODE_VERTEX_PAINT));
}

void BKE_paint_cavity_curve_preset(Paint *paint, int preset)
{
  CurveMapping *cumap = nullptr;
  CurveMap *cuma = nullptr;

  if (!paint->cavity_curve) {
    paint->cavity_curve = BKE_curvemapping_add(1, 0, 0, 1, 1);
  }
  cumap = paint->cavity_curve;
  cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
  cumap->preset = preset;

  cuma = cumap->cm;
  BKE_curvemap_reset(cuma, &cumap->clipr, cumap->preset, CURVEMAP_SLOPE_POSITIVE);
  BKE_curvemapping_changed(cumap, false);
}

eObjectMode BKE_paint_object_mode_from_paintmode(const PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
      return OB_MODE_SCULPT;
    case PaintMode::Vertex:
      return OB_MODE_VERTEX_PAINT;
    case PaintMode::Weight:
      return OB_MODE_WEIGHT_PAINT;
    case PaintMode::Texture2D:
    case PaintMode::Texture3D:
      return OB_MODE_TEXTURE_PAINT;
    case PaintMode::SculptCurves:
      return OB_MODE_SCULPT_CURVES;
    case PaintMode::GPencil:
      return OB_MODE_PAINT_GPENCIL_LEGACY;
    case PaintMode::SculptGreasePencil:
      return OB_MODE_SCULPT_GPENCIL_LEGACY;
    case PaintMode::Invalid:
    default:
      return OB_MODE_OBJECT;
  }
}

bool BKE_paint_ensure(Main *bmain, ToolSettings *ts, Paint **r_paint)
{
  Paint *paint = nullptr;
  if (*r_paint) {
    if (!(*r_paint)->runtime.initialized) {
      /* Currently only image painting is initialized this way, others have to be allocated. */
      BLI_assert(ELEM(*r_paint, (Paint *)&ts->imapaint));

      paint_runtime_init(ts, *r_paint);
      BKE_paint_brush_set_default(bmain, *r_paint);
      BKE_paint_eraser_brush_set_default(bmain, *r_paint);
    }
    else {
      BLI_assert(ELEM(*r_paint,
                      /* Cast is annoying, but prevent nullptr-pointer access. */
                      (Paint *)ts->gp_paint,
                      (Paint *)ts->gp_vertexpaint,
                      (Paint *)ts->gp_sculptpaint,
                      (Paint *)ts->gp_weightpaint,
                      (Paint *)ts->sculpt,
                      (Paint *)ts->vpaint,
                      (Paint *)ts->wpaint,
                      (Paint *)ts->curves_sculpt,
                      (Paint *)&ts->imapaint));
#ifndef NDEBUG
      Paint paint_test = **r_paint;
      paint_runtime_init(ts, *r_paint);
      /* Swap so debug doesn't hide errors when release fails. */
      std::swap(**r_paint, paint_test);
      BLI_assert(paint_test.runtime.ob_mode == (*r_paint)->runtime.ob_mode);
#endif
    }
    paint_brush_set_from_asset_reference(bmain, *r_paint);
    paint_eraser_brush_set_from_asset_reference(bmain, *r_paint);
    return true;
  }

  if (((VPaint **)r_paint == &ts->vpaint) || ((VPaint **)r_paint == &ts->wpaint)) {
    VPaint *data = MEM_cnew<VPaint>(__func__);
    paint = &data->paint;
  }
  else if ((Sculpt **)r_paint == &ts->sculpt) {
    Sculpt *data = MEM_cnew<Sculpt>(__func__);

    *data = *DNA_struct_default_get(Sculpt);

    paint = &data->paint;
  }
  else if ((GpPaint **)r_paint == &ts->gp_paint) {
    GpPaint *data = MEM_cnew<GpPaint>(__func__);
    paint = &data->paint;
  }
  else if ((GpVertexPaint **)r_paint == &ts->gp_vertexpaint) {
    GpVertexPaint *data = MEM_cnew<GpVertexPaint>(__func__);
    paint = &data->paint;
  }
  else if ((GpSculptPaint **)r_paint == &ts->gp_sculptpaint) {
    GpSculptPaint *data = MEM_cnew<GpSculptPaint>(__func__);
    paint = &data->paint;
  }
  else if ((GpWeightPaint **)r_paint == &ts->gp_weightpaint) {
    GpWeightPaint *data = MEM_cnew<GpWeightPaint>(__func__);
    paint = &data->paint;
  }
  else if ((CurvesSculpt **)r_paint == &ts->curves_sculpt) {
    CurvesSculpt *data = MEM_cnew<CurvesSculpt>(__func__);
    paint = &data->paint;
  }
  else if (*r_paint == &ts->imapaint.paint) {
    paint = &ts->imapaint.paint;
  }

  paint->flags |= PAINT_SHOW_BRUSH;

  *r_paint = paint;

  paint_runtime_init(ts, paint);
  BKE_paint_brush_set_default(bmain, paint);
  BKE_paint_eraser_brush_set_default(bmain, paint);

  return false;
}

void BKE_paint_init(Main *bmain, Scene *sce, PaintMode mode, const uchar col[3])
{
  UnifiedPaintSettings *ups = &sce->toolsettings->unified_paint_settings;

  BKE_paint_ensure_from_paintmode(bmain, sce, mode);
  Paint *paint = BKE_paint_get_active_from_paintmode(sce, mode);

  copy_v3_v3_uchar(paint->paint_cursor_col, col);
  paint->paint_cursor_col[3] = 128;
  ups->last_stroke_valid = false;
  zero_v3(ups->average_stroke_accum);
  ups->average_stroke_counter = 0;
  if (!paint->cavity_curve) {
    BKE_paint_cavity_curve_preset(paint, CURVE_PRESET_LINE);
  }
}

void BKE_paint_free(Paint *paint)
{
  BKE_curvemapping_free(paint->cavity_curve);
  MEM_delete(paint->brush_asset_reference);
  MEM_delete(paint->eraser_brush_asset_reference);
}

void BKE_paint_copy(const Paint *src, Paint *dst, const int flag)
{
  dst->brush = src->brush;
  dst->cavity_curve = BKE_curvemapping_copy(src->cavity_curve);

  if (src->brush_asset_reference) {
    dst->brush_asset_reference = MEM_new<AssetWeakReference>(__func__,
                                                             *src->brush_asset_reference);
  }
  if (src->eraser_brush_asset_reference) {
    dst->eraser_brush_asset_reference = MEM_new<AssetWeakReference>(
        __func__, *src->eraser_brush_asset_reference);
  }

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus((ID *)dst->palette);
  }
}

void BKE_paint_stroke_get_average(const Scene *scene, const Object *ob, float stroke[3])
{
  const UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  if (ups->last_stroke_valid && ups->average_stroke_counter > 0) {
    float fac = 1.0f / ups->average_stroke_counter;
    mul_v3_v3fl(stroke, ups->average_stroke_accum, fac);
  }
  else {
    copy_v3_v3(stroke, ob->object_to_world().location());
  }
}

void BKE_paint_blend_write(BlendWriter *writer, Paint *paint)
{
  if (paint->cavity_curve) {
    BKE_curvemapping_blend_write(writer, paint->cavity_curve);
  }
  if (paint->brush_asset_reference) {
    BKE_asset_weak_reference_write(writer, paint->brush_asset_reference);
  }
  if (paint->eraser_brush_asset_reference) {
    BKE_asset_weak_reference_write(writer, paint->eraser_brush_asset_reference);
  }
}

void BKE_paint_blend_read_data(BlendDataReader *reader, const Scene *scene, Paint *paint)
{
  BLO_read_struct(reader, CurveMapping, &paint->cavity_curve);
  if (paint->cavity_curve) {
    BKE_curvemapping_blend_read(reader, paint->cavity_curve);
  }
  else {
    BKE_paint_cavity_curve_preset(paint, CURVE_PRESET_LINE);
  }

  BLO_read_struct(reader, AssetWeakReference, &paint->brush_asset_reference);
  if (paint->brush_asset_reference) {
    BKE_asset_weak_reference_read(reader, paint->brush_asset_reference);
  }

  BLO_read_struct(reader, AssetWeakReference, &paint->eraser_brush_asset_reference);
  if (paint->eraser_brush_asset_reference) {
    BKE_asset_weak_reference_read(reader, paint->eraser_brush_asset_reference);
  }

  paint->paint_cursor = nullptr;
  paint_runtime_init(scene->toolsettings, paint);
}

bool paint_is_grid_face_hidden(const blender::BoundedBitSpan grid_hidden,
                               const int gridsize,
                               const int x,
                               const int y)
{
  return grid_hidden[CCG_grid_xy_to_index(gridsize, x, y)] ||
         grid_hidden[CCG_grid_xy_to_index(gridsize, x + 1, y)] ||
         grid_hidden[CCG_grid_xy_to_index(gridsize, x + 1, y + 1)] ||
         grid_hidden[CCG_grid_xy_to_index(gridsize, x, y + 1)];
}

bool paint_is_bmesh_face_hidden(const BMFace *f)
{
  BMLoop *l_iter;
  BMLoop *l_first;

  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    if (BM_elem_flag_test(l_iter->v, BM_ELEM_HIDDEN)) {
      return true;
    }
  } while ((l_iter = l_iter->next) != l_first);

  return false;
}

float paint_grid_paint_mask(const GridPaintMask *gpm, uint level, uint x, uint y)
{
  int factor = BKE_ccg_factor(level, gpm->level);
  int gridsize = BKE_ccg_gridsize(gpm->level);

  return gpm->data[(y * factor) * gridsize + (x * factor)];
}

/* Threshold to move before updating the brush rotation, reduces jitter. */
static float paint_rake_rotation_spacing(const UnifiedPaintSettings & /*ups*/, const Brush &brush)
{
  return brush.sculpt_brush_type == SCULPT_BRUSH_TYPE_CLAY_STRIPS ? 1.0f : 20.0f;
}

void paint_update_brush_rake_rotation(UnifiedPaintSettings &ups,
                                      const Brush &brush,
                                      float rotation)
{
  ups.brush_rotation = rotation;

  if (brush.mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE) {
    ups.brush_rotation_sec = rotation;
  }
  else {
    ups.brush_rotation_sec = 0.0f;
  }
}

static bool paint_rake_rotation_active(const MTex &mtex)
{
  return mtex.tex && mtex.brush_angle_mode & MTEX_ANGLE_RAKE;
}

static const bool paint_rake_rotation_active(const Brush &brush, PaintMode paint_mode)
{
  return paint_rake_rotation_active(brush.mtex) || paint_rake_rotation_active(brush.mask_mtex) ||
         BKE_brush_has_cube_tip(&brush, paint_mode);
}

bool paint_calculate_rake_rotation(UnifiedPaintSettings &ups,
                                   const Brush &brush,
                                   const float mouse_pos[2],
                                   const PaintMode paint_mode,
                                   bool stroke_has_started)
{
  bool ok = false;
  if (paint_rake_rotation_active(brush, paint_mode)) {
    float r = paint_rake_rotation_spacing(ups, brush);
    float rotation;

    /* Use a smaller limit if the stroke hasn't started to prevent excessive pre-roll. */
    if (!stroke_has_started) {
      r = min_ff(r, 4.0f);
    }

    float dpos[2];
    sub_v2_v2v2(dpos, mouse_pos, ups.last_rake);

    /* Limit how often we update the angle to prevent jitter. */
    if (len_squared_v2(dpos) >= r * r) {
      rotation = atan2f(dpos[1], dpos[0]) + float(0.5f * M_PI);

      copy_v2_v2(ups.last_rake, mouse_pos);

      ups.last_rake_angle = rotation;

      paint_update_brush_rake_rotation(ups, brush, rotation);
      ok = true;
    }
    /* Make sure we reset here to the last rotation to avoid accumulating
     * values in case a random rotation is also added. */
    else {
      paint_update_brush_rake_rotation(ups, brush, ups.last_rake_angle);
      ok = false;
    }
  }
  else {
    ups.brush_rotation = ups.brush_rotation_sec = 0.0f;
    ok = true;
  }
  return ok;
}

void BKE_sculptsession_free_deformMats(SculptSession *ss)
{
  ss->deform_cos = {};
  ss->deform_imats = {};
  ss->vert_normals_deform = {};
  ss->face_normals_deform = {};
}

void BKE_sculptsession_free_vwpaint_data(SculptSession *ss)
{
  if (ss->mode_type == OB_MODE_WEIGHT_PAINT) {
    MEM_SAFE_FREE(ss->mode.wpaint.alpha_weight);
    if (ss->mode.wpaint.dvert_prev) {
      BKE_defvert_array_free_elems(ss->mode.wpaint.dvert_prev, ss->totvert);
      MEM_freeN(ss->mode.wpaint.dvert_prev);
      ss->mode.wpaint.dvert_prev = nullptr;
    }
  }
}

/**
 * Write out the sculpt dynamic-topology #BMesh to the #Mesh.
 */
static void sculptsession_bm_to_me_update_data_only(Object *ob, bool reorder)
{
  SculptSession &ss = *ob->sculpt;

  if (ss.bm) {
    if (ob->data) {
      if (reorder) {
        BM_log_mesh_elems_reorder(ss.bm, ss.bm_log);
      }
      BMeshToMeshParams params{};
      params.calc_object_remap = false;
      BM_mesh_bm_to_me(nullptr, ss.bm, static_cast<Mesh *>(ob->data), &params);
    }
  }
}

void BKE_sculptsession_bm_to_me(Object *ob, bool reorder)
{
  if (ob && ob->sculpt) {
    sculptsession_bm_to_me_update_data_only(ob, reorder);

    /* Ensure the objects evaluated mesh doesn't hold onto arrays
     * now realloc'd in the mesh #34473. */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
}

void BKE_sculptsession_free_pbvh(SculptSession *ss)
{
  if (!ss) {
    return;
  }

  blender::bke::pbvh::free(ss->pbvh);
  ss->vert_to_face_map = {};
  ss->edge_to_face_offsets = {};
  ss->edge_to_face_indices = {};
  ss->edge_to_face_map = {};
  ss->vert_to_edge_offsets = {};
  ss->vert_to_edge_indices = {};
  ss->vert_to_edge_map = {};

  ss->preview_verts = {};

  ss->vertex_info.boundary.clear_and_shrink();
  ss->fake_neighbors.fake_neighbor_index = {};

  ss->clear_active_vert();
}

void BKE_sculptsession_bm_to_me_for_render(Object *object)
{
  if (object && object->sculpt) {
    if (object->sculpt->bm) {
      /* Ensure no points to old arrays are stored in DM
       *
       * Apparently, we could not use DEG_id_tag_update
       * here because this will lead to the while object
       * surface to disappear, so we'll release DM in place.
       */
      BKE_object_free_derived_caches(object);

      sculptsession_bm_to_me_update_data_only(object, false);

      /* In contrast with sculptsession_bm_to_me no need in
       * DAG tag update here - derived mesh was freed and
       * old pointers are nowhere stored.
       */
    }
  }
}

void BKE_sculptsession_free(Object *ob)
{
  if (ob && ob->sculpt) {
    SculptSession *ss = ob->sculpt;

    BKE_sculpt_attribute_destroy_temporary_all(ob);

    if (ss->bm) {
      BKE_sculptsession_bm_to_me(ob, true);
      BM_mesh_free(ss->bm);
    }

    BKE_sculptsession_free_pbvh(ss);

    MEM_delete(ss);

    ob->sculpt = nullptr;
  }
}

SculptSession::SculptSession()
{
  /* Code expects attribute domains to be zero initialized. Avoid exposing #AttrDomain definition
   * in header. */
  for (const int i : blender::IndexRange(ARRAY_SIZE(this->temp_attributes))) {
    this->temp_attributes[i].domain = blender::bke::AttrDomain::Point;
  }
}

SculptSession::~SculptSession()
{
  if (this->bm_log) {
    BM_log_free(this->bm_log);
  }

  if (this->tex_pool) {
    BKE_image_pool_free(this->tex_pool);
  }

  BKE_sculptsession_free_vwpaint_data(this);

  MEM_SAFE_FREE(this->last_paint_canvas_key);
}

PBVHVertRef SculptSession::active_vert_ref() const
{
  if (std::holds_alternative<int>(active_vert_)) {
    return {std::get<int>(active_vert_)};
  }
  if (std::holds_alternative<SubdivCCGCoord>(active_vert_)) {
    const CCGKey key = BKE_subdiv_ccg_key_top_level(*this->subdiv_ccg);
    const int index = std::get<SubdivCCGCoord>(active_vert_).to_index(key);
    return {index};
  }
  if (std::holds_alternative<BMVert *>(active_vert_)) {
    return {reinterpret_cast<intptr_t>(std::get<BMVert *>(active_vert_))};
  }
  return {PBVH_REF_NONE};
}

ActiveVert SculptSession::active_vert() const
{
  return active_vert_;
}

int SculptSession::active_vert_index() const
{
  if (std::holds_alternative<int>(active_vert_)) {
    return std::get<int>(active_vert_);
  }
  if (std::holds_alternative<SubdivCCGCoord>(active_vert_)) {
    const SubdivCCGCoord coord = std::get<SubdivCCGCoord>(active_vert_);
    return coord.to_index(BKE_subdiv_ccg_key_top_level(*this->subdiv_ccg));
  }
  if (std::holds_alternative<BMVert *>(active_vert_)) {
    BMVert *bm_vert = std::get<BMVert *>(active_vert_);
    return BM_elem_index_get(bm_vert);
  }

  return -1;
}

blender::float3 SculptSession::active_vert_position(const Depsgraph &depsgraph,
                                                    const Object &object) const
{
  if (std::holds_alternative<int>(active_vert_)) {
    const Span<float3> positions = blender::bke::pbvh::vert_positions_eval(depsgraph, object);
    return positions[std::get<int>(active_vert_)];
  }
  if (std::holds_alternative<SubdivCCGCoord>(active_vert_)) {
    const CCGKey key = BKE_subdiv_ccg_key_top_level(*this->subdiv_ccg);
    const SubdivCCGCoord coord = std::get<SubdivCCGCoord>(active_vert_);

    return CCG_grid_elem_co(key, this->subdiv_ccg->grids[coord.grid_index], coord.x, coord.y);
  }
  if (std::holds_alternative<BMVert *>(active_vert_)) {
    BMVert *bm_vert = std::get<BMVert *>(active_vert_);
    return bm_vert->co;
  }

  BLI_assert_unreachable();
  return float3(std::numeric_limits<float>::infinity());
}

void SculptSession::clear_active_vert()
{
  active_vert_ = {};
}

void SculptSession::set_active_vert(const ActiveVert vert)
{
  active_vert_ = vert;
}

static MultiresModifierData *sculpt_multires_modifier_get(const Scene *scene,
                                                          Object *ob,
                                                          const bool auto_create_mdisps)
{
  Mesh *mesh = (Mesh *)ob->data;
  ModifierData *md;
  VirtualModifierData virtual_modifier_data;

  if (ob->sculpt && ob->sculpt->bm) {
    /* Can't combine multires and dynamic topology. */
    return nullptr;
  }

  bool need_mdisps = false;

  if (!CustomData_get_layer(&mesh->corner_data, CD_MDISPS)) {
    if (!auto_create_mdisps) {
      /* Multires can't work without displacement layer. */
      return nullptr;
    }
    need_mdisps = true;
  }

  /* Weight paint operates on original vertices, and needs to treat multires as regular modifier
   * to make it so that pbvh::Tree vertices are at the multires surface. */
  if ((ob->mode & OB_MODE_SCULPT) == 0) {
    return nullptr;
  }

  for (md = BKE_modifiers_get_virtual_modifierlist(ob, &virtual_modifier_data); md; md = md->next)
  {
    if (md->type == eModifierType_Multires) {
      MultiresModifierData *mmd = (MultiresModifierData *)md;

      if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
        continue;
      }

      if (mmd->sculptlvl > 0 && !(mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
        if (need_mdisps) {
          CustomData_add_layer(&mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, mesh->corners_num);
        }

        return mmd;
      }

      return nullptr;
    }
  }

  return nullptr;
}

MultiresModifierData *BKE_sculpt_multires_active(const Scene *scene, Object *ob)
{
  return sculpt_multires_modifier_get(scene, ob, false);
}

/* Checks if there are any supported deformation modifiers active */
static bool sculpt_modifiers_active(Scene *scene, Sculpt *sd, Object *ob)
{
  ModifierData *md;
  Mesh *mesh = (Mesh *)ob->data;
  VirtualModifierData virtual_modifier_data;

  if (ob->sculpt->bm || BKE_sculpt_multires_active(scene, ob)) {
    return false;
  }

  /* Non-locked shape keys could be handled in the same way as deformed mesh. */
  if ((ob->shapeflag & OB_SHAPE_LOCK) == 0 && mesh->key && ob->shapenr) {
    return true;
  }

  md = BKE_modifiers_get_virtual_modifierlist(ob, &virtual_modifier_data);

  /* Exception for shape keys because we can edit those. */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(static_cast<ModifierType>(md->type));
    if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
      continue;
    }
    if (md->type == eModifierType_Multires && (ob->mode & OB_MODE_SCULPT)) {
      MultiresModifierData *mmd = (MultiresModifierData *)md;
      if (!(mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
        continue;
      }
    }
    if (md->type == eModifierType_ShapeKey) {
      continue;
    }

    if (mti->type == ModifierTypeType::OnlyDeform) {
      return true;
    }
    if ((sd->flags & SCULPT_ONLY_DEFORM) == 0) {
      return true;
    }
  }

  return false;
}

static void sculpt_update_object(Depsgraph *depsgraph,
                                 Object *ob,
                                 Object *ob_eval,
                                 bool is_paint_tool)
{
  Scene *scene = DEG_get_input_scene(depsgraph);
  Sculpt *sd = scene->toolsettings->sculpt;
  SculptSession &ss = *ob->sculpt;
  Mesh *mesh_orig = BKE_object_get_original_mesh(ob);
  /* Use the "unchecked" function, because this code also runs as part of the depsgraph node that
   * evaluates the object's geometry. So from perspective of the depsgraph, the mesh is not fully
   * evaluated yet. */
  Mesh *mesh_eval = BKE_object_get_evaluated_mesh_unchecked(ob_eval);
  MultiresModifierData *mmd = sculpt_multires_modifier_get(scene, ob, true);
  const bool use_face_sets = (ob->mode & OB_MODE_SCULPT) != 0;

  BLI_assert(mesh_eval != nullptr);

  /* This is for handling a newly opened file with no object visible,
   * causing `mesh_eval == nullptr`. */
  if (mesh_eval == nullptr) {
    return;
  }

  ss.depsgraph = depsgraph;

  ss.deform_modifiers_active = sculpt_modifiers_active(scene, sd, ob);

  ss.building_vp_handle = false;

  ss.scene = scene;

  ss.shapekey_active = (mmd == nullptr) ? BKE_keyblock_from_object(ob) : nullptr;

  /* NOTE: Weight pPaint require mesh info for loop lookup, but it never uses multires code path,
   * so no extra checks is needed here. */
  if (mmd) {
    ss.multires.active = true;
    ss.multires.modifier = mmd;
    ss.multires.level = mmd->sculptlvl;
    ss.totvert = mesh_eval->verts_num;
    ss.faces_num = mesh_eval->faces_num;
    ss.totfaces = mesh_orig->faces_num;

    /* These are assigned to the base mesh in Multires. This is needed because Face Sets operators
     * and tools use the Face Sets data from the base mesh when Multires is active. */
    ss.faces = mesh_orig->faces();
    ss.corner_verts = mesh_orig->corner_verts();
  }
  else {
    ss.totvert = mesh_orig->verts_num;
    ss.faces_num = mesh_orig->faces_num;
    ss.totfaces = mesh_orig->faces_num;
    ss.faces = mesh_orig->faces();
    ss.corner_verts = mesh_orig->corner_verts();
    ss.multires.active = false;
    ss.multires.modifier = nullptr;
    ss.multires.level = 0;
  }

  /* Sculpt Face Sets. */
  if (use_face_sets) {
    ss.face_sets = static_cast<const int *>(
        CustomData_get_layer_named(&mesh_orig->face_data, CD_PROP_INT32, ".sculpt_face_set"));
  }
  else {
    ss.face_sets = nullptr;
  }

  ss.hide_poly = (bool *)CustomData_get_layer_named(
      &mesh_orig->face_data, CD_PROP_BOOL, ".hide_poly");

  ss.subdiv_ccg = mesh_eval->runtime->subdiv_ccg.get();

  blender::bke::pbvh::Tree *pbvh = BKE_sculpt_object_pbvh_ensure(depsgraph, ob);
  BLI_assert(pbvh == ss.pbvh.get());
  UNUSED_VARS_NDEBUG(pbvh);

  sculpt_attribute_update_refs(ob, ss.pbvh->type());

  if (ob->type == OB_MESH) {
    ss.vert_to_face_map = mesh_orig->vert_to_face_map();
  }

  if (ss.deform_modifiers_active) {
    /* Painting doesn't need crazyspace, use already evaluated mesh coordinates if possible. */
    bool used_me_eval = false;

    if (ob->mode & (OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT)) {
      const Mesh *me_eval_deform = BKE_object_get_mesh_deform_eval(ob_eval);

      /* If the fully evaluated mesh has the same topology as the deform-only version, use it.
       * This matters because crazyspace evaluation is very restrictive and excludes even modifiers
       * that simply recompute vertex weights (which can even include Geometry Nodes). */
      if (me_eval_deform->faces_num == mesh_eval->faces_num &&
          me_eval_deform->corners_num == mesh_eval->corners_num &&
          me_eval_deform->verts_num == mesh_eval->verts_num)
      {
        BKE_sculptsession_free_deformMats(&ss);

        BLI_assert(me_eval_deform->verts_num == mesh_orig->verts_num);

        ss.deform_cos = mesh_eval->vert_positions();
        BKE_pbvh_vert_coords_apply(*ss.pbvh, ss.deform_cos);

        used_me_eval = true;
      }
    }

    /* We depend on the deform coordinates not being updated in the middle of a stroke. This array
     * eventually gets cleared inside BKE_sculpt_update_object_before_eval.
     * See #126713 for more information. */
    if (ss.deform_cos.is_empty() && !used_me_eval) {
      BKE_sculptsession_free_deformMats(&ss);

      BKE_crazyspace_build_sculpt(depsgraph, scene, ob, ss.deform_imats, ss.deform_cos);
      BKE_pbvh_vert_coords_apply(*ss.pbvh, ss.deform_cos);

      for (blender::float3x3 &matrix : ss.deform_imats) {
        matrix = blender::math::invert(matrix);
      }
    }
  }
  else {
    BKE_sculptsession_free_deformMats(&ss);
  }

  if (ss.shapekey_active != nullptr && ss.deform_cos.is_empty()) {
    ss.deform_cos = Span(static_cast<const float3 *>(ss.shapekey_active->data),
                         mesh_orig->verts_num);
  }

  /* if pbvh is deformed, key block is already applied to it */
  if (ss.shapekey_active) {
    if (ss.deform_cos.is_empty()) {
      const Span key_data(static_cast<const float3 *>(ss.shapekey_active->data),
                          mesh_orig->verts_num);

      if (key_data.data() != nullptr) {
        BKE_pbvh_vert_coords_apply(*ss.pbvh, key_data);
        if (ss.deform_cos.is_empty()) {
          ss.deform_cos = key_data;
        }
      }
    }
  }

  if (is_paint_tool) {
    /* We should rebuild the PBVH_pixels when painting canvas changes.
     *
     * The relevant changes are stored/encoded in the paint canvas key.
     * These include the active uv map, and resolutions. */
    if (U.experimental.use_sculpt_texture_paint && ss.pbvh) {
      char *paint_canvas_key = BKE_paint_canvas_key_get(&scene->toolsettings->paint_mode, ob);
      if (ss.last_paint_canvas_key == nullptr ||
          !STREQ(paint_canvas_key, ss.last_paint_canvas_key))
      {
        MEM_SAFE_FREE(ss.last_paint_canvas_key);
        ss.last_paint_canvas_key = paint_canvas_key;
        BKE_pbvh_mark_rebuild_pixels(*ss.pbvh);
      }
      else {
        MEM_freeN(paint_canvas_key);
      }
    }

    /* We could be more precise when we have access to the active tool. */
    const bool use_paint_slots = (ob->mode & OB_MODE_SCULPT) != 0;
    if (use_paint_slots) {
      BKE_texpaint_slots_refresh_object(scene, ob);
    }
  }
}

void BKE_sculpt_update_object_before_eval(Object *ob_eval)
{
  using namespace blender;
  /* Update before mesh evaluation in the dependency graph. */
  SculptSession *ss = ob_eval->sculpt;

  if (ss && ss->building_vp_handle == false) {
    if (!ss->cache && !ss->filter_cache && !ss->expand_cache) {
      /* Avoid performing the following normal update for Multires, as it causes race conditions
       * and other intermittent crashes with shared meshes.
       * See !125268 and #125157 for more information. */
      if (ss->pbvh && ss->pbvh->type() != blender::bke::pbvh::Type::Grids) {
        /* pbvh::Tree nodes may contain dirty normal tags. To avoid losing that information when
         * the pbvh::Tree is deleted, make sure all tagged geometry normals are up to date.
         * See #122947 for more information. */
        blender::bke::pbvh::update_normals_from_eval(*ob_eval, *ss->pbvh);
      }
      /* We free pbvh on changes, except in the middle of drawing a stroke
       * since it can't deal with changing PVBH node organization, we hope
       * topology does not change in the meantime .. weak. */
      BKE_sculptsession_free_pbvh(ss);

      BKE_sculptsession_free_deformMats(ob_eval->sculpt);

      /* In vertex/weight paint, force maps to be rebuilt. */
      BKE_sculptsession_free_vwpaint_data(ob_eval->sculpt);
    }
    else if (ss->pbvh) {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(*ss->pbvh, memory);
      switch (ss->pbvh->type()) {
        case bke::pbvh::Type::Mesh: {
          MutableSpan<bke::pbvh::MeshNode> nodes = ss->pbvh->nodes<bke::pbvh::MeshNode>();
          node_mask.foreach_index([&](const int i) { BKE_pbvh_node_mark_update(nodes[i]); });
          break;
        }
        case bke::pbvh::Type::Grids: {
          MutableSpan<bke::pbvh::GridsNode> nodes = ss->pbvh->nodes<bke::pbvh::GridsNode>();
          node_mask.foreach_index([&](const int i) { BKE_pbvh_node_mark_update(nodes[i]); });
          break;
        }
        case bke::pbvh::Type::BMesh: {
          MutableSpan<bke::pbvh::BMeshNode> nodes = ss->pbvh->nodes<bke::pbvh::BMeshNode>();
          node_mask.foreach_index([&](const int i) { BKE_pbvh_node_mark_update(nodes[i]); });
          break;
        }
      }
    }
  }
}

void BKE_sculpt_update_object_after_eval(Depsgraph *depsgraph, Object *ob_eval)
{
  /* Update after mesh evaluation in the dependency graph, to rebuild pbvh::Tree or
   * other data when modifiers change the mesh. */
  Object *ob_orig = DEG_get_original_object(ob_eval);

  sculpt_update_object(depsgraph, ob_orig, ob_eval, false);
}

void BKE_sculpt_color_layer_create_if_needed(Object *object)
{
  using namespace blender;
  using namespace blender::bke;
  Mesh *orig_me = BKE_object_get_original_mesh(object);

  if (BKE_color_attribute_supported(*orig_me, orig_me->active_color_attribute)) {
    return;
  }

  AttributeOwner owner = AttributeOwner::from_id(&orig_me->id);
  const std::string unique_name = BKE_attribute_calc_unique_name(owner, "Color");
  if (!orig_me->attributes_for_write().add(
          unique_name, AttrDomain::Point, CD_PROP_COLOR, AttributeInitDefaultValue()))
  {
    return;
  }

  BKE_id_attributes_active_color_set(&orig_me->id, unique_name.c_str());
  BKE_id_attributes_default_color_set(&orig_me->id, unique_name.c_str());
  DEG_id_tag_update(&orig_me->id, ID_RECALC_GEOMETRY_ALL_MODES);
  BKE_mesh_tessface_clear(orig_me);
}

void BKE_sculpt_update_object_for_edit(Depsgraph *depsgraph, Object *ob_orig, bool is_paint_tool)
{
  BLI_assert(ob_orig == DEG_get_original_object(ob_orig));

  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob_orig);

  sculpt_update_object(depsgraph, ob_orig, ob_eval, is_paint_tool);
}

void BKE_sculpt_hide_poly_pointer_update(Object &object)
{
  const Mesh &mesh = *static_cast<const Mesh *>(object.data);
  object.sculpt->hide_poly = static_cast<const bool *>(
      CustomData_get_layer_named(&mesh.face_data, CD_PROP_BOOL, ".hide_poly"));
}

void BKE_sculpt_mask_layers_ensure(Depsgraph *depsgraph,
                                   Main *bmain,
                                   Object *ob,
                                   MultiresModifierData *mmd)
{
  using namespace blender;
  using namespace blender::bke;
  Mesh *mesh = static_cast<Mesh *>(ob->data);
  const OffsetIndices faces = mesh->faces();
  const Span<int> corner_verts = mesh->corner_verts();
  MutableAttributeAccessor attributes = mesh->attributes_for_write();

  /* if multires is active, create a grid paint mask layer if there
   * isn't one already */
  if (mmd && !CustomData_has_layer(&mesh->corner_data, CD_GRID_PAINT_MASK)) {
    int level = max_ii(1, mmd->sculptlvl);
    int gridsize = BKE_ccg_gridsize(level);
    int gridarea = gridsize * gridsize;

    GridPaintMask *gmask = static_cast<GridPaintMask *>(CustomData_add_layer(
        &mesh->corner_data, CD_GRID_PAINT_MASK, CD_SET_DEFAULT, mesh->corners_num));

    for (int i = 0; i < mesh->corners_num; i++) {
      GridPaintMask *gpm = &gmask[i];

      gpm->level = level;
      gpm->data = static_cast<float *>(
          MEM_callocN(sizeof(float) * gridarea, "GridPaintMask.data"));
    }

    /* If vertices already have mask, copy into multires data. */
    if (const VArray<float> mask = *attributes.lookup<float>(".sculpt_mask", AttrDomain::Point)) {
      const VArraySpan<float> mask_span(mask);
      for (const int i : faces.index_range()) {
        const IndexRange face = faces[i];

        /* Mask center. */
        float avg = 0.0f;
        for (const int vert : corner_verts.slice(face)) {
          avg += mask_span[vert];
        }
        avg /= float(face.size());

        /* Fill in multires mask corner. */
        for (const int corner : face) {
          GridPaintMask *gpm = &gmask[corner];
          const int vert = corner_verts[corner];
          const int prev = corner_verts[mesh::face_corner_prev(face, corner)];
          const int next = corner_verts[mesh::face_corner_next(face, corner)];

          gpm->data[0] = avg;
          gpm->data[1] = (mask_span[vert] + mask_span[next]) * 0.5f;
          gpm->data[2] = (mask_span[vert] + mask_span[prev]) * 0.5f;
          gpm->data[3] = mask_span[vert];
        }
      }
    }
    /* The evaluated multires CCG must be updated to contain the new data. */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    if (depsgraph) {
      BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
    }
  }
  else {
    attributes.add<float>(".sculpt_mask", AttrDomain::Point, AttributeInitDefaultValue());
  }
}

void BKE_sculpt_toolsettings_data_ensure(Main *bmain, Scene *scene)
{
  BKE_paint_ensure(bmain, scene->toolsettings, (Paint **)&scene->toolsettings->sculpt);

  Sculpt *sd = scene->toolsettings->sculpt;

  const Sculpt *defaults = DNA_struct_default_get(Sculpt);

  /* We have file versioning code here for historical
   * reasons.  Don't add more checks here, do it properly
   * in blenloader.
   */
  if (sd->automasking_start_normal_limit == 0.0f) {
    sd->automasking_start_normal_limit = defaults->automasking_start_normal_limit;
    sd->automasking_start_normal_falloff = defaults->automasking_start_normal_falloff;

    sd->automasking_view_normal_limit = defaults->automasking_view_normal_limit;
    sd->automasking_view_normal_falloff = defaults->automasking_view_normal_limit;
  }

  if (sd->detail_percent == 0.0f) {
    sd->detail_percent = defaults->detail_percent;
  }
  if (sd->constant_detail == 0.0f) {
    sd->constant_detail = defaults->constant_detail;
  }
  if (sd->detail_size == 0.0f) {
    sd->detail_size = defaults->detail_size;
  }

  /* Set sane default tiling offsets. */
  if (!sd->paint.tile_offset[0]) {
    sd->paint.tile_offset[0] = 1.0f;
  }
  if (!sd->paint.tile_offset[1]) {
    sd->paint.tile_offset[1] = 1.0f;
  }
  if (!sd->paint.tile_offset[2]) {
    sd->paint.tile_offset[2] = 1.0f;
  }

  if (!sd->automasking_cavity_curve || !sd->automasking_cavity_curve_op) {
    BKE_sculpt_check_cavity_curves(sd);
  }
}

static bool check_sculpt_object_deformed(Object *object, const bool for_construction)
{
  bool deformed = false;

  /* Active modifiers means extra deformation, which can't be handled correct
   * on birth of pbvh::Tree and sculpt "layer" levels, so use pbvh::Tree only for internal brush
   * stuff and show final evaluated mesh so user would see actual object shape. */
  deformed |= object->sculpt->deform_modifiers_active;

  if (for_construction) {
    deformed |= object->sculpt->shapekey_active != nullptr;
  }
  else {
    /* As in case with modifiers, we can't synchronize deformation made against
     * pbvh::Tree and non-locked keyblock, so also use pbvh::Tree only for brushes and
     * final DM to give final result to user. */
    deformed |= object->sculpt->shapekey_active && (object->shapeflag & OB_SHAPE_LOCK) == 0;
  }

  return deformed;
}

void BKE_sculpt_sync_face_visibility_to_grids(const Mesh &mesh, SubdivCCG &subdiv_ccg)
{
  using namespace blender;
  using namespace blender::bke;

  const AttributeAccessor attributes = mesh.attributes();
  const VArray<bool> hide_poly = *attributes.lookup_or_default<bool>(
      ".hide_poly", AttrDomain::Face, false);
  if (hide_poly.is_single() && !hide_poly.get_internal_single()) {
    BKE_subdiv_ccg_grid_hidden_free(subdiv_ccg);
    return;
  }

  const OffsetIndices<int> faces = mesh.faces();

  const VArraySpan<bool> hide_poly_span(hide_poly);
  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);
  threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      const bool face_hidden = hide_poly_span[i];
      for (const int corner : faces[i]) {
        grid_hidden[corner].set_all(face_hidden);
      }
    }
  });
}

namespace blender::bke {

static std::unique_ptr<pbvh::Tree> build_pbvh_for_dynamic_topology(Object *ob)
{
  BMesh &bm = *ob->sculpt->bm;
  BM_data_layer_ensure_named(&bm, &bm.vdata, CD_PROP_INT32, ".sculpt_dyntopo_node_id_vertex");
  BM_data_layer_ensure_named(&bm, &bm.pdata, CD_PROP_INT32, ".sculpt_dyntopo_node_id_face");

  return pbvh::build_bmesh(&bm);
}

static std::unique_ptr<pbvh::Tree> build_pbvh_from_regular_mesh(Object *ob,
                                                                const Mesh *me_eval_deform)
{
  const Mesh &mesh = *BKE_object_get_original_mesh(ob);
  std::unique_ptr<pbvh::Tree> pbvh = pbvh::build_mesh(mesh);

  const bool is_deformed = check_sculpt_object_deformed(ob, true);
  if (is_deformed && me_eval_deform != nullptr) {
    BKE_pbvh_vert_coords_apply(*pbvh, me_eval_deform->vert_positions());
  }

  return pbvh;
}

static std::unique_ptr<pbvh::Tree> build_pbvh_from_ccg(Object *ob, SubdivCCG &subdiv_ccg)
{
  const Mesh &base_mesh = *BKE_mesh_from_object(ob);
  BKE_sculpt_sync_face_visibility_to_grids(base_mesh, subdiv_ccg);

  return pbvh::build_grids(base_mesh, subdiv_ccg);
}

}  // namespace blender::bke

blender::bke::pbvh::Tree *BKE_sculpt_object_pbvh_ensure(Depsgraph *depsgraph, Object *ob)
{
  using namespace blender::bke;
  if (ob->sculpt == nullptr) {
    return nullptr;
  }

  if (ob->sculpt->pbvh) {
    return ob->sculpt->pbvh.get();
  }

  if (ob->sculpt->bm != nullptr) {
    /* Sculpting on a BMesh (dynamic-topology) gets a special pbvh::Tree. */
    ob->sculpt->pbvh = build_pbvh_for_dynamic_topology(ob);
  }
  else {
    Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
    Mesh *mesh_eval = static_cast<Mesh *>(object_eval->data);
    if (mesh_eval->runtime->subdiv_ccg != nullptr) {
      ob->sculpt->pbvh = build_pbvh_from_ccg(ob, *mesh_eval->runtime->subdiv_ccg);
    }
    else if (ob->type == OB_MESH) {
      const Mesh *me_eval_deform = BKE_object_get_mesh_deform_eval(object_eval);
      ob->sculpt->pbvh = build_pbvh_from_regular_mesh(ob, me_eval_deform);
    }
  }

  sculpt_attribute_update_refs(ob, ob->sculpt->pbvh->type());
  return ob->sculpt->pbvh.get();
}

blender::bke::pbvh::Tree *BKE_object_sculpt_pbvh_get(Object *object)
{
  if (!object->sculpt) {
    return nullptr;
  }
  return object->sculpt->pbvh.get();
}

bool BKE_object_sculpt_use_dyntopo(const Object *object)
{
  return object->sculpt && object->sculpt->bm;
}

bool BKE_sculptsession_use_pbvh_draw(const Object *ob, const RegionView3D *rv3d)
{
  SculptSession *ss = ob->sculpt;
  if (ss == nullptr || ss->pbvh == nullptr || ss->mode_type != OB_MODE_SCULPT) {
    return false;
  }

  if (ss->pbvh->type() == blender::bke::pbvh::Type::Mesh) {
    /* Regular mesh only draws from pbvh::Tree without modifiers and shape keys, or for
     * external engines that do not have access to the pbvh::Tree like Eevee does. */
    const bool external_engine = rv3d && rv3d->view_render != nullptr;
    return !(ss->shapekey_active || ss->deform_modifiers_active || external_engine);
  }

  /* Multires and dyntopo always draw directly from the pbvh::Tree. */
  return true;
}

/* Returns the Face Set random color for rendering in the overlay given its ID and a color seed. */
#define GOLDEN_RATIO_CONJUGATE 0.618033988749895f
void BKE_paint_face_set_overlay_color_get(const int face_set, const int seed, uchar r_color[4])
{
  float rgba[4];
  float random_mod_hue = GOLDEN_RATIO_CONJUGATE * (face_set + (seed % 10));
  random_mod_hue = random_mod_hue - floorf(random_mod_hue);
  const float random_mod_sat = BLI_hash_int_01(face_set + seed + 1);
  const float random_mod_val = BLI_hash_int_01(face_set + seed + 2);
  hsv_to_rgb(random_mod_hue,
             0.6f + (random_mod_sat * 0.25f),
             1.0f - (random_mod_val * 0.35f),
             &rgba[0],
             &rgba[1],
             &rgba[2]);
  rgba_float_to_uchar(r_color, rgba);
}

int BKE_sculptsession_vertex_count(const SculptSession *ss)
{
  if (ss->bm) {
    return ss->bm->totvert;
  }
  if (ss->subdiv_ccg) {
    return ss->subdiv_ccg->grids.size() * BKE_subdiv_ccg_key_top_level(*ss->subdiv_ccg).grid_area;
  }
  return ss->totvert;
}

/**
 * Returns pointer to a CustomData associated with a given domain, if
 * one exists.  If not nullptr is returned (this may happen with e.g.
 * multires and #AttrDomain::Point).
 */
static CustomData *sculpt_get_cdata(Object *ob, AttrDomain domain)
{
  SculptSession &ss = *ob->sculpt;

  if (ss.bm) {
    switch (domain) {
      case AttrDomain::Point:
        return &ss.bm->vdata;
      case AttrDomain::Face:
        return &ss.bm->pdata;
      default:
        BLI_assert_unreachable();
        return nullptr;
    }
  }
  else {
    Mesh *mesh = BKE_object_get_original_mesh(ob);

    switch (domain) {
      case AttrDomain::Point:
        /* Cannot get vertex domain for multires grids. */
        if (ss.pbvh && ss.pbvh->type() == blender::bke::pbvh::Type::Grids) {
          return nullptr;
        }

        return &mesh->vert_data;
      case AttrDomain::Face:
        return &mesh->face_data;
      default:
        BLI_assert_unreachable();
        return nullptr;
    }
  }
}

static int sculpt_attr_elem_count_get(Object *ob, AttrDomain domain)
{
  const SculptSession &ss = *ob->sculpt;

  switch (domain) {
    case AttrDomain::Point:
      return BKE_sculptsession_vertex_count(&ss);
      break;
    case AttrDomain::Face:
      return ss.totfaces;
      break;
    default:
      BLI_assert_unreachable();
      return 0;
  }
}

static bool sculpt_attribute_create(SculptSession *ss,
                                    Object *ob,
                                    AttrDomain domain,
                                    eCustomDataType proptype,
                                    const char *name,
                                    SculptAttribute *out,
                                    const SculptAttributeParams *params,
                                    blender::bke::pbvh::Type pbvhtype,
                                    bool flat_array_for_bmesh)
{
  Mesh *mesh = BKE_object_get_original_mesh(ob);

  bool simple_array = params->simple_array;
  bool permanent = params->permanent;

  out->params = *params;
  out->proptype = proptype;
  out->domain = domain;
  STRNCPY_UTF8(out->name, name);

  /* Force non-CustomData simple_array mode if not pbvh::Type::Mesh. */
  if (pbvhtype == blender::bke::pbvh::Type::Grids ||
      (pbvhtype == blender::bke::pbvh::Type::BMesh && flat_array_for_bmesh))
  {
    if (permanent) {
      printf(
          "%s: error: tried to make permanent customdata in multires or bmesh mode; will make "
          "local "
          "array "
          "instead.\n",
          __func__);
      permanent = (out->params.permanent = false);
    }

    simple_array = true;
  }

  BLI_assert(!(simple_array && permanent));

  int totelem = sculpt_attr_elem_count_get(ob, domain);

  if (simple_array) {
    int elemsize = CustomData_sizeof(proptype);

    out->data = MEM_calloc_arrayN(totelem, elemsize, __func__);

    out->data_for_bmesh = ss->bm != nullptr;
    out->simple_array = true;
    out->bmesh_cd_offset = -1;
    out->layer = nullptr;
    out->elem_size = elemsize;
    out->used = true;
    out->elem_num = totelem;

    return true;
  }

  out->simple_array = false;

  if (BMesh *bm = ss->bm) {
    CustomData *cdata = nullptr;
    out->data_for_bmesh = true;

    switch (domain) {
      case AttrDomain::Point:
        cdata = &bm->vdata;
        break;
      case AttrDomain::Face:
        cdata = &bm->pdata;
        break;
      default:
        out->used = false;
        return false;
    }

    BLI_assert(CustomData_get_named_layer_index(cdata, proptype, name) == -1);

    BM_data_layer_add_named(bm, cdata, proptype, name);
    int index = CustomData_get_named_layer_index(cdata, proptype, name);

    if (!permanent) {
      cdata->layers[index].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
    }

    out->data = nullptr;
    out->layer = cdata->layers + index;
    out->bmesh_cd_offset = out->layer->offset;
    out->elem_size = CustomData_sizeof(proptype);
  }
  else {
    CustomData *cdata = nullptr;

    switch (domain) {
      case AttrDomain::Point:
        cdata = &mesh->vert_data;
        break;
      case AttrDomain::Face:
        cdata = &mesh->face_data;
        break;
      default:
        out->used = false;
        return false;
    }

    BLI_assert(CustomData_get_named_layer_index(cdata, proptype, name) == -1);

    CustomData_add_layer_named(cdata, proptype, CD_SET_DEFAULT, totelem, name);
    int index = CustomData_get_named_layer_index(cdata, proptype, name);

    if (!permanent) {
      cdata->layers[index].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
    }

    out->layer = cdata->layers + index;
    out->data = out->layer->data;
    out->data_for_bmesh = false;
    out->bmesh_cd_offset = -1;
    out->elem_size = CustomData_get_elem_size(out->layer);
  }
  /* GRIDS should have been handled as simple arrays. */

  out->used = true;
  out->elem_num = totelem;

  return true;
}

static bool sculpt_attr_update(Object *ob,
                               SculptAttribute *attr,
                               blender::bke::pbvh::Type pbvh_type)
{
  SculptSession *ss = ob->sculpt;
  int elem_num = sculpt_attr_elem_count_get(ob, attr->domain);

  bool bad = false;

  if (attr->data) {
    bad = attr->elem_num != elem_num;
  }

  /* Check if we are a coerced simple array and shouldn't be. */
  bad |= attr->simple_array && !attr->params.simple_array &&
         !ELEM(pbvh_type, blender::bke::pbvh::Type::Grids, blender::bke::pbvh::Type::BMesh);

  CustomData *cdata = sculpt_get_cdata(ob, attr->domain);
  if (cdata && !attr->simple_array) {
    int layer_index = CustomData_get_named_layer_index(cdata, attr->proptype, attr->name);

    bad |= layer_index == -1;
    bad |= (ss->bm != nullptr) != attr->data_for_bmesh;

    if (!bad) {
      if (attr->data_for_bmesh) {
        attr->bmesh_cd_offset = cdata->layers[layer_index].offset;
      }
      else {
        attr->data = cdata->layers[layer_index].data;
      }
    }
  }

  if (bad) {
    if (attr->simple_array) {
      MEM_SAFE_FREE(attr->data);
    }

    sculpt_attribute_create(ss,
                            ob,
                            attr->domain,
                            attr->proptype,
                            attr->name,
                            attr,
                            &attr->params,
                            pbvh_type,
                            attr->data_for_bmesh);
  }

  return bad;
}

static SculptAttribute *sculpt_get_cached_layer(SculptSession *ss,
                                                AttrDomain domain,
                                                eCustomDataType proptype,
                                                const char *name)
{
  for (int i = 0; i < SCULPT_MAX_ATTRIBUTES; i++) {
    SculptAttribute *attr = ss->temp_attributes + i;

    if (attr->used && STREQ(attr->name, name) && attr->proptype == proptype &&
        attr->domain == domain)
    {

      return attr;
    }
  }

  return nullptr;
}

static SculptAttribute *sculpt_alloc_attr(SculptSession *ss)
{
  for (int i = 0; i < SCULPT_MAX_ATTRIBUTES; i++) {
    if (!ss->temp_attributes[i].used) {
      memset((void *)(ss->temp_attributes + i), 0, sizeof(SculptAttribute));
      ss->temp_attributes[i].used = true;

      return ss->temp_attributes + i;
    }
  }

  BLI_assert_unreachable();
  return nullptr;
}

/* The pbvh::Tree is NOT guaranteed to exist at the point of this method being called. */
static SculptAttribute *sculpt_attribute_get_ex(Object *ob,
                                                blender::bke::pbvh::Type pbvhtype,
                                                AttrDomain domain,
                                                eCustomDataType proptype,
                                                const char *name)
{
  SculptSession *ss = ob->sculpt;
  /* See if attribute is cached in ss->temp_attributes. */
  SculptAttribute *attr = sculpt_get_cached_layer(ss, domain, proptype, name);

  if (attr) {
    if (sculpt_attr_update(ob, attr, pbvhtype)) {
      sculpt_attribute_update_refs(ob, pbvhtype);
    }

    return attr;
  }

  /* Does attribute exist in CustomData layout? */
  CustomData *cdata = sculpt_get_cdata(ob, domain);
  if (cdata) {
    int index = CustomData_get_named_layer_index(cdata, proptype, name);

    if (index != -1) {
      int totelem = 0;

      switch (domain) {
        case AttrDomain::Point:
          totelem = BKE_sculptsession_vertex_count(ss);
          break;
        case AttrDomain::Face:
          totelem = ss->totfaces;
          break;
        default:
          BLI_assert_unreachable();
          break;
      }

      attr = sculpt_alloc_attr(ss);

      attr->used = true;
      attr->domain = domain;
      attr->proptype = proptype;
      attr->data = cdata->layers[index].data;
      attr->bmesh_cd_offset = cdata->layers[index].offset;
      attr->elem_num = totelem;
      attr->layer = cdata->layers + index;
      attr->elem_size = CustomData_get_elem_size(attr->layer);

      STRNCPY_UTF8(attr->name, name);
      return attr;
    }
  }

  return nullptr;
}

SculptAttribute *BKE_sculpt_attribute_get(Object *ob,
                                          AttrDomain domain,
                                          eCustomDataType proptype,
                                          const char *name)
{
  SculptSession *ss = ob->sculpt;
  BLI_assert(ss->pbvh != nullptr);

  return sculpt_attribute_get_ex(ob, ss->pbvh->type(), domain, proptype, name);
}

static SculptAttribute *sculpt_attribute_ensure_ex(Object *ob,
                                                   AttrDomain domain,
                                                   eCustomDataType proptype,
                                                   const char *name,
                                                   const SculptAttributeParams *params,
                                                   blender::bke::pbvh::Type pbvhtype,
                                                   bool flat_array_for_bmesh)
{
  SculptSession *ss = ob->sculpt;
  SculptAttribute *attr = sculpt_attribute_get_ex(ob, pbvhtype, domain, proptype, name);

  if (attr) {
    sculpt_attr_update(ob, attr, pbvhtype);

    /* Since "stroke_only" is not a CustomData flag we have
     * to sync its parameter setting manually. Fixes #104618.
     */
    attr->params.stroke_only = params->stroke_only;

    return attr;
  }

  attr = sculpt_alloc_attr(ss);

  /* Create attribute. */
  sculpt_attribute_create(
      ss, ob, domain, proptype, name, attr, params, pbvhtype, flat_array_for_bmesh);
  sculpt_attribute_update_refs(ob, pbvhtype);

  return attr;
}

SculptAttribute *BKE_sculpt_attribute_ensure(Object *ob,
                                             AttrDomain domain,
                                             eCustomDataType proptype,
                                             const char *name,
                                             const SculptAttributeParams *params)
{
  SculptAttributeParams temp_params = *params;

  return sculpt_attribute_ensure_ex(
      ob, domain, proptype, name, &temp_params, ob->sculpt->pbvh->type(), true);
}

void BKE_sculpt_attributes_destroy_temporary_stroke(Object *ob)
{
  SculptSession *ss = ob->sculpt;

  for (int i = 0; i < SCULPT_MAX_ATTRIBUTES; i++) {
    SculptAttribute *attr = ss->temp_attributes + i;

    if (attr->params.stroke_only) {
      BKE_sculpt_attribute_destroy(ob, attr);
    }
  }
}

static void sculpt_attribute_update_refs(Object *ob, blender::bke::pbvh::Type pbvhtype)
{
  SculptSession *ss = ob->sculpt;

  /* Run twice, in case sculpt_attr_update had to recreate a layer and messed up #BMesh offsets. */
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < SCULPT_MAX_ATTRIBUTES; j++) {
      SculptAttribute *attr = ss->temp_attributes + j;

      if (attr->used) {
        sculpt_attr_update(ob, attr, pbvhtype);
      }
    }
  }
}

void BKE_sculpt_attribute_destroy_temporary_all(Object *ob)
{
  SculptSession *ss = ob->sculpt;

  for (int i = 0; i < SCULPT_MAX_ATTRIBUTES; i++) {
    SculptAttribute *attr = ss->temp_attributes + i;

    if (attr->used && !attr->params.permanent) {
      BKE_sculpt_attribute_destroy(ob, attr);
    }
  }
}

bool BKE_sculpt_attribute_destroy(Object *ob, SculptAttribute *attr)
{
  SculptSession *ss = ob->sculpt;
  AttrDomain domain = attr->domain;

  BLI_assert(attr->used);

  /* Remove from convenience pointer struct. */
  SculptAttribute **ptrs = (SculptAttribute **)&ss->attrs;
  int ptrs_num = sizeof(ss->attrs) / sizeof(void *);

  for (int i = 0; i < ptrs_num; i++) {
    if (ptrs[i] == attr) {
      ptrs[i] = nullptr;
    }
  }

  /* Remove from internal temp_attributes array. */
  for (int i = 0; i < SCULPT_MAX_ATTRIBUTES; i++) {
    SculptAttribute *attr2 = ss->temp_attributes + i;

    if (STREQ(attr2->name, attr->name) && attr2->domain == attr->domain &&
        attr2->proptype == attr->proptype)
    {

      attr2->used = false;
    }
  }

  Mesh *mesh = BKE_object_get_original_mesh(ob);

  if (attr->simple_array) {
    MEM_SAFE_FREE(attr->data);
  }
  else if (ss->bm) {
    CustomData *cdata = attr->domain == AttrDomain::Point ? &ss->bm->vdata : &ss->bm->pdata;

    BM_data_layer_free_named(ss->bm, cdata, attr->name);
  }
  else {
    CustomData *cdata = nullptr;
    int totelem = 0;

    switch (domain) {
      case AttrDomain::Point:
        cdata = ss->bm ? &ss->bm->vdata : &mesh->vert_data;
        totelem = ss->totvert;
        break;
      case AttrDomain::Face:
        cdata = ss->bm ? &ss->bm->pdata : &mesh->face_data;
        totelem = ss->totfaces;
        break;
      default:
        BLI_assert_unreachable();
        return false;
    }

    /* We may have been called after destroying ss->bm in which case attr->layer
     * might be invalid.
     */
    int layer_i = CustomData_get_named_layer_index(cdata, attr->proptype, attr->name);
    if (layer_i != 0) {
      CustomData_free_layer(cdata, attr->proptype, totelem, layer_i);
    }

    if (ss->pbvh) {
      /* If the pbvh::Tree doesn't exist, we cannot update references
       * This can occur when all the attributes are being deleted. */
      sculpt_attribute_update_refs(ob, ss->pbvh->type());
    }
  }

  attr->data = nullptr;
  attr->used = false;

  return true;
}
