/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BLI_array_utils.hh"
#include "BLI_assert.h"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.h"
#include "BKE_preview_image.hh"
#include "BKE_report.hh"

#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_object.hh"
#include "ED_view3d.hh"

#include "GEO_join_geometries.hh"
#include "GEO_reorder.hh"
#include "GEO_set_curve_type.hh"
#include "GEO_smooth_curves.hh"
#include "GEO_subdivide_curves.hh"

#include "UI_resources.hh"
#include <limits>

namespace blender::ed::greasepencil {

/* -------------------------------------------------------------------- */
/** \name Smooth Stroke Operator
 * \{ */

static int grease_pencil_stroke_smooth_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const int iterations = RNA_int_get(op->ptr, "iterations");
  const float influence = RNA_float_get(op->ptr, "factor");
  const bool keep_shape = RNA_boolean_get(op->ptr, "keep_shape");
  const bool smooth_ends = RNA_boolean_get(op->ptr, "smooth_ends");

  const bool smooth_position = RNA_boolean_get(op->ptr, "smooth_position");
  const bool smooth_radius = RNA_boolean_get(op->ptr, "smooth_radius");
  const bool smooth_opacity = RNA_boolean_get(op->ptr, "smooth_opacity");

  if (!(smooth_position || smooth_radius || smooth_opacity)) {
    /* There's nothing to be smoothed, return. */
    return OPERATOR_FINISHED;
  }

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();
    const OffsetIndices points_by_curve = curves.points_by_curve();
    const VArray<bool> cyclic = curves.cyclic();
    const VArray<bool> point_selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);

    if (smooth_position) {
      bke::GSpanAttributeWriter positions = attributes.lookup_for_write_span("position");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       keep_shape,
                                       positions.span);
      positions.finish();
      changed = true;
    }
    if (smooth_opacity && info.drawing.opacities().is_span()) {
      bke::GSpanAttributeWriter opacities = attributes.lookup_for_write_span("opacity");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       false,
                                       opacities.span);
      opacities.finish();
      changed = true;
    }
    if (smooth_radius && info.drawing.radii().is_span()) {
      bke::GSpanAttributeWriter radii = attributes.lookup_for_write_span("radius");
      geometry::smooth_curve_attribute(strokes,
                                       points_by_curve,
                                       point_selection,
                                       cyclic,
                                       iterations,
                                       influence,
                                       smooth_ends,
                                       false,
                                       radii.span);
      radii.finish();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_smooth(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Smooth Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_smooth";
  ot->description = "Smooth selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_smooth_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Smooth parameters. */
  prop = RNA_def_int(ot->srna, "iterations", 10, 1, 100, "Iterations", "", 1, 30);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_float(ot->srna, "factor", 1.0f, 0.0f, 1.0f, "Factor", "", 0.0f, 1.0f);
  RNA_def_boolean(ot->srna, "smooth_ends", false, "Smooth Endpoints", "");
  RNA_def_boolean(ot->srna, "keep_shape", false, "Keep Shape", "");

  RNA_def_boolean(ot->srna, "smooth_position", true, "Position", "");
  RNA_def_boolean(ot->srna, "smooth_radius", true, "Radius", "");
  RNA_def_boolean(ot->srna, "smooth_opacity", false, "Opacity", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Simplify Stroke Operator
 * \{ */

static float dist_to_interpolated(
    float3 pos, float3 posA, float3 posB, float val, float valA, float valB)
{
  float dist1 = math::distance_squared(posA, pos);
  float dist2 = math::distance_squared(posB, pos);

  if (dist1 + dist2 > 0) {
    float interpolated_val = interpf(valB, valA, dist1 / (dist1 + dist2));
    return math::distance(interpolated_val, val);
  }
  return 0.0f;
}

static int64_t stroke_simplify(const IndexRange points,
                               const bool cyclic,
                               const float epsilon,
                               const FunctionRef<float(int64_t, int64_t, int64_t)> dist_function,
                               MutableSpan<bool> points_to_delete)
{
  int64_t total_points_to_delete = 0;
  const Span<bool> curve_selection = points_to_delete.slice(points);
  if (!curve_selection.contains(true)) {
    return total_points_to_delete;
  }

  const bool is_last_segment_selected = (curve_selection.first() && curve_selection.last());

  const Vector<IndexRange> selection_ranges = array_utils::find_all_ranges(curve_selection, true);
  threading::parallel_for(
      selection_ranges.index_range(), 1024, [&](const IndexRange range_of_ranges) {
        for (const IndexRange range : selection_ranges.as_span().slice(range_of_ranges)) {
          total_points_to_delete += ramer_douglas_peucker_simplify(
              range.shift(points.start()), epsilon, dist_function, points_to_delete);
        }
      });

  /* For cyclic curves, simplify the last segment. */
  if (cyclic && points.size() > 2 && is_last_segment_selected) {
    const float dist = dist_function(points.last(1), points.first(), points.last());
    if (dist <= epsilon) {
      points_to_delete[points.last()] = true;
      total_points_to_delete++;
    }
  }

  return total_points_to_delete;
}

static int grease_pencil_stroke_simplify_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const float epsilon = RNA_float_get(op->ptr, "factor");

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    const Span<float3> positions = curves.positions();
    const VArray<float> radii = info.drawing.radii();

    /* Distance functions for `ramer_douglas_peucker_simplify`. */
    const auto dist_function_positions =
        [positions](int64_t first_index, int64_t last_index, int64_t index) {
          const float dist_position = dist_to_line_v3(
              positions[index], positions[first_index], positions[last_index]);
          return dist_position;
        };
    const auto dist_function_positions_and_radii =
        [positions, radii](int64_t first_index, int64_t last_index, int64_t index) {
          const float dist_position = dist_to_line_v3(
              positions[index], positions[first_index], positions[last_index]);
          const float dist_radii = dist_to_interpolated(positions[index],
                                                        positions[first_index],
                                                        positions[last_index],
                                                        radii[index],
                                                        radii[first_index],
                                                        radii[last_index]);
          return math::max(dist_position, dist_radii);
        };

    const VArray<bool> cyclic = curves.cyclic();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
        ".selection", bke::AttrDomain::Point, true);

    /* Mark all points in the editable curves to be deleted. */
    Array<bool> points_to_delete(curves.points_num(), false);
    bke::curves::fill_points(points_by_curve, strokes, true, points_to_delete.as_mutable_span());

    std::atomic<int64_t> total_points_to_delete = 0;
    if (radii.is_single()) {
      strokes.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        total_points_to_delete += stroke_simplify(points,
                                                  cyclic[curve_i],
                                                  epsilon,
                                                  dist_function_positions,
                                                  points_to_delete.as_mutable_span());
      });
    }
    else if (radii.is_span()) {
      strokes.foreach_index([&](const int64_t curve_i) {
        const IndexRange points = points_by_curve[curve_i];
        total_points_to_delete += stroke_simplify(points,
                                                  cyclic[curve_i],
                                                  epsilon,
                                                  dist_function_positions_and_radii,
                                                  points_to_delete.as_mutable_span());
      });
    }

    if (total_points_to_delete > 0) {
      IndexMaskMemory memory;
      curves.remove_points(IndexMask::from_bools(points_to_delete, memory), {});
      info.drawing.tag_topology_changed();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_simplify(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Simplify Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_simplify";
  ot->description = "Simplify selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_simplify_exec;
  ot->poll = editable_grease_pencil_point_selection_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  prop = RNA_def_float(ot->srna, "factor", 0.01f, 0.0f, 100.0f, "Factor", "", 0.0f, 100.0f);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Operator
 * \{ */

static bke::CurvesGeometry remove_points_and_split(const bke::CurvesGeometry &curves,
                                                   const IndexMask &mask)
{
  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  const VArray<bool> src_cyclic = curves.cyclic();

  Array<bool> points_to_delete(curves.points_num());
  mask.to_bools(points_to_delete.as_mutable_span());
  const int total_points = points_to_delete.as_span().count(false);

  /* Return if deleting everything. */
  if (total_points == 0) {
    return {};
  }

  int curr_dst_point_id = 0;
  Array<int> dst_to_src_point(total_points);
  Vector<int> dst_curve_counts;
  Vector<int> dst_to_src_curve;
  Vector<bool> dst_cyclic;

  for (const int curve_i : curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<bool> curve_points_to_delete = points_to_delete.as_span().slice(points);
    const bool curve_cyclic = src_cyclic[curve_i];

    /* Note, these ranges start at zero and needed to be shifted by `points.first()` */
    const Vector<IndexRange> ranges_to_keep = array_utils::find_all_ranges(curve_points_to_delete,
                                                                           false);

    if (ranges_to_keep.is_empty()) {
      continue;
    }

    const bool is_last_segment_selected = curve_cyclic && ranges_to_keep.first().first() == 0 &&
                                          ranges_to_keep.last().last() == points.size() - 1;
    const bool is_curve_self_joined = is_last_segment_selected && ranges_to_keep.size() != 1;
    const bool is_cyclic = ranges_to_keep.size() == 1 && is_last_segment_selected;

    IndexRange range_ids = ranges_to_keep.index_range();
    /* Skip the first range because it is joined to the end of the last range. */
    for (const int range_i : ranges_to_keep.index_range().drop_front(is_curve_self_joined)) {
      const IndexRange range = ranges_to_keep[range_i];

      int count = range.size();
      for (const int src_point : range.shift(points.first())) {
        dst_to_src_point[curr_dst_point_id++] = src_point;
      }

      /* Join the first range to the end of the last range. */
      if (is_curve_self_joined && range_i == range_ids.last()) {
        const IndexRange first_range = ranges_to_keep[range_ids.first()];
        for (const int src_point : first_range.shift(points.first())) {
          dst_to_src_point[curr_dst_point_id++] = src_point;
        }
        count += first_range.size();
      }

      dst_curve_counts.append(count);
      dst_to_src_curve.append(curve_i);
      dst_cyclic.append(is_cyclic);
    }
  }

  const int total_curves = dst_to_src_curve.size();

  bke::CurvesGeometry dst_curves(total_points, total_curves);

  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &curves.vertex_group_names);

  MutableSpan<int> new_curve_offsets = dst_curves.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  const bke::AttributeAccessor src_attributes = curves.attributes();

  /* Transfer curve attributes. */
  gather_attributes(
      src_attributes, bke::AttrDomain::Curve, {}, {"cyclic"}, dst_to_src_curve, dst_attributes);
  array_utils::copy(dst_cyclic.as_span(), dst_curves.cyclic_for_write());

  /* Transfer point attributes. */
  gather_attributes(
      src_attributes, bke::AttrDomain::Point, {}, {}, dst_to_src_point, dst_attributes);

  dst_curves.update_curve_types();
  dst_curves.remove_attributes_based_on_types();

  return dst_curves;
}

static int grease_pencil_delete_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask elements = ed::greasepencil::retrieve_editable_and_selected_elements(
        *object, info.drawing, info.layer_index, selection_domain, memory);
    if (elements.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (selection_domain == bke::AttrDomain::Curve) {
      curves.remove_curves(elements, {});
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      curves = remove_points_and_split(curves, elements);
    }
    info.drawing.tag_topology_changed();
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_delete(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Delete";
  ot->idname = "GREASE_PENCIL_OT_delete";
  ot->description = "Delete selected strokes or points";

  /* Callbacks. */
  ot->exec = grease_pencil_delete_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dissolve Points Operator
 * \{ */

enum class DissolveMode : int8_t {
  /** Dissolve all selected points. */
  POINTS = 0,
  /** Dissolve between selected points. */
  BETWEEN = 1,
  /** Dissolve unselected points. */
  UNSELECT = 2,
};

static const EnumPropertyItem prop_dissolve_types[] = {
    {int(DissolveMode::POINTS), "POINTS", 0, "Dissolve", "Dissolve selected points"},
    {int(DissolveMode::BETWEEN),
     "BETWEEN",
     0,
     "Dissolve Between",
     "Dissolve points between selected points"},
    {int(DissolveMode::UNSELECT),
     "UNSELECT",
     0,
     "Dissolve Unselect",
     "Dissolve all unselected points"},
    {0, nullptr, 0, nullptr, nullptr},
};

static Array<bool> get_points_to_dissolve(bke::CurvesGeometry &curves,
                                          const IndexMask &mask,
                                          const DissolveMode mode)
{
  const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
      ".selection", bke::AttrDomain::Point, true);

  Array<bool> points_to_dissolve(curves.points_num(), false);
  selection.materialize(mask, points_to_dissolve);

  if (mode == DissolveMode::POINTS) {
    return points_to_dissolve;
  }

  /* Both `between` and `unselect` have the unselected point being the ones dissolved so we need
   * to invert. */
  BLI_assert(ELEM(mode, DissolveMode::BETWEEN, DissolveMode::UNSELECT));

  const OffsetIndices<int> points_by_curve = curves.points_by_curve();
  /* Because we are going to invert, these become the points to keep. */
  MutableSpan<bool> points_to_keep = points_to_dissolve.as_mutable_span();

  threading::parallel_for(curves.curves_range(), 128, [&](const IndexRange range) {
    for (const int64_t curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      const Span<bool> curve_selection = points_to_dissolve.as_span().slice(points);
      /* The unselected curves should not be dissolved. */
      if (!curve_selection.contains(true)) {
        points_to_keep.slice(points).fill(true);
        continue;
      }

      /* `between` is just `unselect` but with the first and last segments not getting
       * dissolved. */
      if (mode != DissolveMode::BETWEEN) {
        continue;
      }

      const Vector<IndexRange> deselection_ranges = array_utils::find_all_ranges(curve_selection,
                                                                                 false);

      if (deselection_ranges.size() != 0) {
        const IndexRange first_range = deselection_ranges.first().shift(points.first());
        const IndexRange last_range = deselection_ranges.last().shift(points.first());

        /* Ranges should only be fill if the first/last point matches the start/end point
         * of the segment. */
        if (first_range.first() == points.first()) {
          points_to_keep.slice(first_range).fill(true);
        }
        if (last_range.last() == points.last()) {
          points_to_keep.slice(last_range).fill(true);
        }
      }
    }
  });

  array_utils::invert_booleans(points_to_dissolve);

  return points_to_dissolve;
}

static int grease_pencil_dissolve_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const DissolveMode mode = DissolveMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (curves.points_num() == 0) {
      return;
    }

    IndexMaskMemory memory;
    const IndexMask points = ed::greasepencil::retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, memory);
    if (points.is_empty()) {
      return;
    }

    const Array<bool> points_to_dissolve = get_points_to_dissolve(curves, points, mode);
    if (points_to_dissolve.as_span().contains(true)) {
      curves.remove_points(IndexMask::from_bools(points_to_dissolve, memory), {});
      info.drawing.tag_topology_changed();
      changed = true;
    }
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_dissolve(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Dissolve";
  ot->idname = "GREASE_PENCIL_OT_dissolve";
  ot->description = "Delete selected points without splitting strokes";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_dissolve_exec;
  ot->poll = editable_grease_pencil_point_selection_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = prop = RNA_def_enum(ot->srna,
                                 "type",
                                 prop_dissolve_types,
                                 0,
                                 "Type",
                                 "Method used for dissolving stroke points");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_GPENCIL);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Delete Frame Operator
 * \{ */

enum class DeleteFrameMode : int8_t {
  /** Delete the active frame for the current layer. */
  ACTIVE_FRAME = 0,
  /** Delete the active frames for all layers. */
  ALL_FRAMES = 1,
};

static const EnumPropertyItem prop_greasepencil_deleteframe_types[] = {
    {int(DeleteFrameMode::ACTIVE_FRAME),
     "ACTIVE_FRAME",
     0,
     "Active Frame",
     "Deletes current frame in the active layer"},
    {int(DeleteFrameMode::ALL_FRAMES),
     "ALL_FRAMES",
     0,
     "All Active Frames",
     "Delete active frames for all layers"},
    {0, nullptr, 0, nullptr, nullptr},
};

static int grease_pencil_delete_frame_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const int current_frame = scene->r.cfra;

  const DeleteFrameMode mode = DeleteFrameMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  if (mode == DeleteFrameMode::ACTIVE_FRAME && grease_pencil.has_active_layer()) {
    bke::greasepencil::Layer &layer = *grease_pencil.get_active_layer();
    if (layer.is_editable() && layer.start_frame_at(current_frame)) {
      changed |= grease_pencil.remove_frames(layer, {*layer.start_frame_at(current_frame)});
    }
  }
  else if (mode == DeleteFrameMode::ALL_FRAMES) {
    for (bke::greasepencil::Layer *layer : grease_pencil.layers_for_write()) {
      if (layer->is_editable() && layer->start_frame_at(current_frame)) {
        changed |= grease_pencil.remove_frames(*layer, {*layer->start_frame_at(current_frame)});
      }
    }
  }

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_delete_frame(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Delete Frame";
  ot->idname = "GREASE_PENCIL_OT_delete_frame";
  ot->description = "Delete Grease Pencil Frame(s)";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_delete_frame_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = prop = RNA_def_enum(ot->srna,
                                 "type",
                                 prop_greasepencil_deleteframe_types,
                                 0,
                                 "Type",
                                 "Method used for deleting Grease Pencil frames");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Material Set Operator
 * \{ */

static int grease_pencil_stroke_material_set_exec(bContext *C, wmOperator *op)
{
  using namespace blender;
  Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  Material *ma = nullptr;
  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "material", name);

  int material_index = object->actcol - 1;

  if (name[0] != '\0') {
    ma = reinterpret_cast<Material *>(BKE_libblock_find_name(bmain, ID_MA, name));
    if (ma == nullptr) {
      BKE_reportf(op->reports, RPT_WARNING, TIP_("Material '%s' could not be found"), name);
      return OPERATOR_CANCELLED;
    }

    /* Find slot index. */
    material_index = BKE_object_material_index_get(object, ma);
  }

  if (material_index == -1) {
    return OPERATOR_CANCELLED;
  }

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    bke::SpanAttributeWriter<int> materials =
        curves.attributes_for_write().lookup_or_add_for_write_span<int>("material_index",
                                                                        bke::AttrDomain::Curve);
    index_mask::masked_fill(materials.span, material_index, strokes);
    materials.finish();
  });

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_material_set(wmOperatorType *ot)
{
  ot->name = "Assign Material";
  ot->idname = "GREASE_PENCIL_OT_stroke_material_set";
  ot->description = "Assign the active material slot to the selected strokes";

  ot->exec = grease_pencil_stroke_material_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_string(
      ot->srna, "material", nullptr, MAX_ID_NAME - 2, "Material", "Name of the material");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Cyclical Set Operator
 * \{ */

enum class CyclicalMode : int8_t {
  /** Sets all strokes to cycle. */
  CLOSE = 0,
  /** Sets all strokes to not cycle. */
  OPEN = 1,
  /** Switches the cyclic state of the strokes. */
  TOGGLE = 2,
};

static const EnumPropertyItem prop_cyclical_types[] = {
    {int(CyclicalMode::CLOSE), "CLOSE", 0, "Close All", ""},
    {int(CyclicalMode::OPEN), "OPEN", 0, "Open All", ""},
    {int(CyclicalMode::TOGGLE), "TOGGLE", 0, "Toggle", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static int grease_pencil_cyclical_set_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const CyclicalMode mode = CyclicalMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (mode == CyclicalMode::OPEN && !curves.attributes().contains("cyclic")) {
      /* Avoid creating unneeded attribute. */
      return;
    }

    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    MutableSpan<bool> cyclic = curves.cyclic_for_write();
    switch (mode) {
      case CyclicalMode::CLOSE:
        index_mask::masked_fill(cyclic, true, strokes);
        break;
      case CyclicalMode::OPEN:
        index_mask::masked_fill(cyclic, false, strokes);
        break;
      case CyclicalMode::TOGGLE:
        array_utils::invert_booleans(cyclic, strokes);
        break;
    }

    /* Remove the attribute if it is empty. */
    if (mode != CyclicalMode::CLOSE) {
      if (array_utils::booleans_mix_calc(curves.cyclic()) == array_utils::BooleanMix::AllFalse) {
        curves.attributes_for_write().remove("cyclic");
      }
    }

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_cyclical_set(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Cyclical State";
  ot->idname = "GREASE_PENCIL_OT_cyclical_set";
  ot->description = "Close or open the selected stroke adding a segment from last to first point";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_cyclical_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = RNA_def_enum(
      ot->srna, "type", prop_cyclical_types, int(CyclicalMode::TOGGLE), "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Active Material Operator
 * \{ */

static int grease_pencil_set_active_material_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  if (object->totcol == 0) {
    return OPERATOR_CANCELLED;
  }

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  for (const MutableDrawingInfo &info : drawings) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      continue;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const VArray<int> materials = *curves.attributes().lookup_or_default<int>(
        "material_index", bke::AttrDomain::Curve, 0);
    object->actcol = materials[strokes.first()] + 1;
    break;
  };

  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_active_material(wmOperatorType *ot)
{
  ot->name = "Set Active Material";
  ot->idname = "GREASE_PENCIL_OT_set_active_material";
  ot->description = "Set the selected stroke material as the active material";

  ot->exec = grease_pencil_set_active_material_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Uniform Thickness Operator
 * \{ */

static int grease_pencil_set_uniform_thickness_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  /* Radius is half of the thickness. */
  const float radius = RNA_float_get(op->ptr, "thickness") * 0.5f;

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    MutableSpan<float> radii = info.drawing.radii_for_write();
    bke::curves::fill_points<float>(points_by_curve, strokes, radius, radii);
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_uniform_thickness(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Uniform Thickness";
  ot->idname = "GREASE_PENCIL_OT_set_uniform_thickness";
  ot->description = "Set all stroke points to same thickness";

  /* Callbacks. */
  ot->exec = grease_pencil_set_uniform_thickness_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_float(
      ot->srna, "thickness", 0.1f, 0.0f, 1000.0f, "Thickness", "Thickness", 0.0f, 1000.0f);
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Set Uniform Opacity Operator
 * \{ */

static int grease_pencil_set_uniform_opacity_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const float opacity = RNA_float_get(op->ptr, "opacity");

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    const OffsetIndices<int> points_by_curve = curves.points_by_curve();
    MutableSpan<float> opacities = info.drawing.opacities_for_write();
    bke::curves::fill_points<float>(points_by_curve, strokes, opacity, opacities);
    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_uniform_opacity(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Uniform Opacity";
  ot->idname = "GREASE_PENCIL_OT_set_uniform_opacity";
  ot->description = "Set all stroke points to same opacity";

  /* Callbacks. */
  ot->exec = grease_pencil_set_uniform_opacity_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_float(ot->srna, "opacity", 1.0f, 0.0f, 1.0f, "Opacity", "", 0.0f, 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Switch Direction Operator
 * \{ */

static int grease_pencil_stroke_switch_direction_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    /* Switch stroke direction. */
    curves.reverse_curves(strokes);

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_switch_direction(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Switch Direction";
  ot->idname = "GREASE_PENCIL_OT_stroke_switch_direction";
  ot->description = "Change direction of the points of the selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_switch_direction_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Curve Caps Operator
 * \{ */

enum class CapsMode : int8_t {
  /** Switches both to Flat. */
  FLAT = 0,
  /** Change only start. */
  START = 1,
  /** Change only end. */
  END = 2,
  /** Switches both to default rounded. */
  ROUND = 3,
};

static void toggle_caps(MutableSpan<int8_t> caps, const IndexMask &strokes)
{
  strokes.foreach_index([&](const int stroke_i) {
    if (caps[stroke_i] == GP_STROKE_CAP_FLAT) {
      caps[stroke_i] = GP_STROKE_CAP_ROUND;
    }
    else {
      caps[stroke_i] = GP_STROKE_CAP_FLAT;
    }
  });
}

static int grease_pencil_caps_set_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const CapsMode mode = CapsMode(RNA_enum_get(op->ptr, "type"));

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

    if (ELEM(mode, CapsMode::ROUND, CapsMode::FLAT)) {
      bke::SpanAttributeWriter<int8_t> start_caps =
          attributes.lookup_or_add_for_write_span<int8_t>("start_cap", bke::AttrDomain::Curve);
      bke::SpanAttributeWriter<int8_t> end_caps = attributes.lookup_or_add_for_write_span<int8_t>(
          "end_cap", bke::AttrDomain::Curve);

      const int8_t flag_set = (mode == CapsMode::ROUND) ? int8_t(GP_STROKE_CAP_TYPE_ROUND) :
                                                          int8_t(GP_STROKE_CAP_TYPE_FLAT);

      index_mask::masked_fill(start_caps.span, flag_set, strokes);
      index_mask::masked_fill(end_caps.span, flag_set, strokes);
      start_caps.finish();
      end_caps.finish();
    }
    else {
      switch (mode) {
        case CapsMode::START: {
          bke::SpanAttributeWriter<int8_t> caps = attributes.lookup_or_add_for_write_span<int8_t>(
              "start_cap", bke::AttrDomain::Curve);
          toggle_caps(caps.span, strokes);
          caps.finish();
          break;
        }
        case CapsMode::END: {
          bke::SpanAttributeWriter<int8_t> caps = attributes.lookup_or_add_for_write_span<int8_t>(
              "end_cap", bke::AttrDomain::Curve);
          toggle_caps(caps.span, strokes);
          caps.finish();
          break;
        }
        case CapsMode::ROUND:
        case CapsMode::FLAT:
          break;
      }
    }

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_caps_set(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_caps_types[] = {
      {int(CapsMode::ROUND), "ROUND", 0, "Rounded", "Set as default rounded"},
      {int(CapsMode::FLAT), "FLAT", 0, "Flat", ""},
      RNA_ENUM_ITEM_SEPR,
      {int(CapsMode::START), "START", 0, "Toggle Start", ""},
      {int(CapsMode::END), "END", 0, "Toggle End", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* Identifiers. */
  ot->name = "Set Curve Caps";
  ot->idname = "GREASE_PENCIL_OT_caps_set";
  ot->description = "Change curve caps mode (rounded or flat)";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_caps_set_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = RNA_def_enum(ot->srna, "type", prop_caps_types, int(CapsMode::ROUND), "Type", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Active Material Operator
 * \{ */

/* Retry enum items with object materials. */
static const EnumPropertyItem *material_enum_itemf(bContext *C,
                                                   PointerRNA * /*ptr*/,
                                                   PropertyRNA * /*prop*/,
                                                   bool *r_free)
{
  Object *ob = CTX_data_active_object(C);
  EnumPropertyItem *item = nullptr, item_tmp = {0};
  int totitem = 0;

  if (ob == nullptr) {
    return rna_enum_dummy_DEFAULT_items;
  }

  /* Existing materials */
  for (const int i : IndexRange(ob->totcol)) {
    if (Material *ma = BKE_object_material_get(ob, i + 1)) {
      item_tmp.identifier = ma->id.name + 2;
      item_tmp.name = ma->id.name + 2;
      item_tmp.value = i + 1;
      item_tmp.icon = ma->preview ? ma->preview->runtime->icon_id : ICON_NONE;

      RNA_enum_item_add(&item, &totitem, &item_tmp);
    }
  }
  RNA_enum_item_end(&item, &totitem);
  *r_free = true;

  return item;
}

static int grease_pencil_set_material_exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const int slot = RNA_enum_get(op->ptr, "slot");

  /* Try to get material slot. */
  if ((slot < 1) || (slot > object->totcol)) {
    return OPERATOR_CANCELLED;
  }

  /* Set active material. */
  object->actcol = slot;

  WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_material(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Active Material";
  ot->idname = "GREASE_PENCIL_OT_set_material";
  ot->description = "Set active material";

  /* callbacks */
  ot->exec = grease_pencil_set_material_exec;
  ot->poll = active_grease_pencil_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Material to use (dynamic enum) */
  ot->prop = RNA_def_enum(ot->srna, "slot", rna_enum_dummy_DEFAULT_items, 0, "Material Slot", "");
  RNA_def_enum_funcs(ot->prop, material_enum_itemf);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Duplicate Operator
 * \{ */

static int grease_pencil_duplicate_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  std::atomic<bool> changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask elements = retrieve_editable_and_selected_elements(
        *object, info.drawing, info.layer_index, selection_domain, memory);
    if (elements.is_empty()) {
      return;
    }

    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    if (selection_domain == bke::AttrDomain::Curve) {
      curves::duplicate_curves(curves, elements);
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      curves::duplicate_points(curves, elements);
    }
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_duplicate(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Duplicate";
  ot->idname = "GREASE_PENCIL_OT_duplicate";
  ot->description = "Duplicate the selected points";

  /* Callbacks. */
  ot->exec = grease_pencil_duplicate_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static int grease_pencil_clean_loose_exec(bContext *C, wmOperator *op)
{
  Object *object = CTX_data_active_object(C);
  Scene &scene = *CTX_data_scene(C);
  const int limit = RNA_int_get(op->ptr, "limit");

  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);

  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    const OffsetIndices<int> points_by_curve = curves.points_by_curve();

    IndexMaskMemory memory;
    const IndexMask editable_strokes = ed::greasepencil::retrieve_editable_strokes(
        *object, info.drawing, info.layer_index, memory);

    const IndexMask curves_to_delete = IndexMask::from_predicate(
        editable_strokes, GrainSize(4096), memory, [&](const int i) {
          return points_by_curve[i].size() <= limit;
        });

    curves.remove_curves(curves_to_delete, {});
  });

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  return OPERATOR_FINISHED;
}

static int grease_pencil_clean_loose_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return WM_operator_props_popup_confirm_ex(
      C, op, event, IFACE_("Remove Loose Points"), IFACE_("Delete"));
}

static void GREASE_PENCIL_OT_clean_loose(wmOperatorType *ot)
{
  ot->name = "Clean Loose Points";
  ot->idname = "GREASE_PENCIL_OT_clean_loose";
  ot->description = "Remove loose points";

  ot->invoke = grease_pencil_clean_loose_invoke;
  ot->exec = grease_pencil_clean_loose_exec;
  ot->poll = active_grease_pencil_layer_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "limit",
              1,
              1,
              INT_MAX,
              "Limit",
              "Number of points to consider stroke as loose",
              1,
              INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Subdivide Operator
 * \{ */

static int gpencil_stroke_subdivide_exec(bContext *C, wmOperator *op)
{
  const int cuts = RNA_int_get(op->ptr, "number_cuts");
  const bool only_selected = RNA_boolean_get(op->ptr, "only_selected");

  std::atomic<bool> changed = false;

  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);

  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    VArray<int> vcuts = {};

    if (selection_domain == bke::AttrDomain::Curve || !only_selected) {
      /* Subdivide entire selected curve, every stroke subdivides to the same cut. */
      vcuts = VArray<int>::ForSingle(cuts, curves.points_num());
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      /* Subdivide between selected points. Only cut between selected points.
       * Make the cut array the same length as point count for specifying
       * cut/uncut for each segment. */
      const VArray<bool> selection = *curves.attributes().lookup_or_default<bool>(
          ".selection", bke::AttrDomain::Point, true);

      const OffsetIndices points_by_curve = curves.points_by_curve();
      const VArray<bool> cyclic = curves.cyclic();

      Array<int> use_cuts(curves.points_num(), 0);

      /* The cut is after each point, so the last point selected wouldn't need to be registered. */
      for (const int curve : curves.curves_range()) {
        /* No need to loop to the last point since the cut is registered on the point before the
         * segment. */
        for (const int point : points_by_curve[curve].drop_back(1)) {
          /* The point itself should be selected. */
          if (!selection[point]) {
            continue;
          }
          /* If the next point in the curve is selected, then cut this segment. */
          if (selection[point + 1]) {
            use_cuts[point] = cuts;
          }
        }
        /* Check for cyclic and selection. */
        if (cyclic[curve]) {
          const int first_point = points_by_curve[curve].first();
          const int last_point = points_by_curve[curve].last();
          if (selection[first_point] && selection[last_point]) {
            use_cuts[last_point] = cuts;
          }
        }
      }
      vcuts = VArray<int>::ForContainer(std::move(use_cuts));
    }

    curves = geometry::subdivide_curves(curves, strokes, vcuts, {});
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_subdivide(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Subdivide Stroke";
  ot->idname = "GREASE_PENCIL_OT_stroke_subdivide";
  ot->description =
      "Subdivide between continuous selected points of the stroke adding a point half way "
      "between "
      "them";

  /* API callbacks. */
  ot->exec = gpencil_stroke_subdivide_exec;
  ot->poll = ed::greasepencil::editable_grease_pencil_poll;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties. */
  prop = RNA_def_int(ot->srna, "number_cuts", 1, 1, 32, "Number of Cuts", "", 1, 5);
  /* Avoid re-using last var because it can cause _very_ high value and annoy users. */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  RNA_def_boolean(ot->srna,
                  "only_selected",
                  true,
                  "Selected Points",
                  "Smooth only selected points in the stroke");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reorder Operator
 * \{ */

enum class ReorderDirection : int8_t {
  /** Move the selected strokes to be drawn in front. */
  TOP = 0,
  /** Increase the draw order of the selected strokes. */
  UP = 1,
  /** Decrease the draw order of the selected strokes. */
  DOWN = 2,
  /** Move the selected strokes to be drawn behind. */
  BOTTOM = 3,
};

static Array<int> get_reordered_indices(const IndexRange universe,
                                        const IndexMask &selected,
                                        const ReorderDirection direction)
{
  Array<int> indices(universe.size());

  if (ELEM(direction, ReorderDirection::UP, ReorderDirection::DOWN)) {
    /* Initialize the indices. */
    array_utils::fill_index_range<int>(indices);
  }

  if (ELEM(direction, ReorderDirection::TOP, ReorderDirection::BOTTOM)) {
    /*
     * Take the selected indices and move them to the start for `Bottom` or the end for `Top`
     * And fill the reset with the unselected indices.
     *
     * Here's a diagram:
     *
     *        Input
     * 0 1 2 3 4 5 6 7 8 9
     *     ^   ^ ^
     *
     *         Top
     * |-----A-----| |-B-|
     * 0 1 3 6 7 8 9 2 4 5
     *               ^ ^ ^
     *
     *        Bottom
     * |-A-| |-----B-----|
     * 2 4 5 0 1 3 6 7 8 9
     * ^ ^ ^
     */

    IndexMaskMemory memory;
    const IndexMask unselected = selected.complement(universe, memory);

    const IndexMask &A = (direction == ReorderDirection::BOTTOM) ? selected : unselected;
    const IndexMask &B = (direction == ReorderDirection::BOTTOM) ? unselected : selected;

    A.to_indices(indices.as_mutable_span().take_front(A.size()));
    B.to_indices(indices.as_mutable_span().take_back(B.size()));
  }
  else if (direction == ReorderDirection::DOWN) {
    selected.foreach_index_optimized<int>([&](const int curve_i, const int pos) {
      /* Check if the curve index is touching the beginning without any gaps. */
      if (curve_i != pos) {
        /* Move a index down by flipping it with the one below it. */
        std::swap(indices[curve_i], indices[curve_i - 1]);
      }
    });
  }
  else if (direction == ReorderDirection::UP) {
    Array<int> selected_indices(selected.size());
    selected.to_indices(selected_indices.as_mutable_span());

    /* Because each index is moving up we need to loop through the indices backwards,
     * starting at the largest. */
    for (const int i : selected_indices.index_range()) {
      const int pos = selected_indices.index_range().last(i);
      const int curve_i = selected_indices[pos];

      /* Check if the curve index is touching the end without any gaps. */
      if (curve_i != universe.last(i)) {
        /* Move a index up by flipping it with the one above it. */
        std::swap(indices[curve_i], indices[curve_i + 1]);
      }
    }
  }

  return indices;
}

static int grease_pencil_stroke_reorder_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const ReorderDirection direction = ReorderDirection(RNA_enum_get(op->ptr, "direction"));

  std::atomic<bool> changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();

    /* Return if everything is selected. */
    if (strokes.size() == curves.curves_num()) {
      return;
    }

    const Array<int> indices = get_reordered_indices(curves.curves_range(), strokes, direction);

    curves = geometry::reorder_curves_geometry(curves, indices, {});
    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_reorder(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_reorder_direction[] = {
      {int(ReorderDirection::TOP), "TOP", 0, "Bring to Front", ""},
      {int(ReorderDirection::UP), "UP", 0, "Bring Forward", ""},
      RNA_ENUM_ITEM_SEPR,
      {int(ReorderDirection::DOWN), "DOWN", 0, "Send Backward", ""},
      {int(ReorderDirection::BOTTOM), "BOTTOM", 0, "Send to Back", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* Identifiers. */
  ot->name = "Reorder";
  ot->idname = "GREASE_PENCIL_OT_reorder";
  ot->description = "Change the display order of the selected strokes";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_reorder_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Simplify parameters. */
  ot->prop = RNA_def_enum(
      ot->srna, "direction", prop_reorder_direction, int(ReorderDirection::TOP), "Direction", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move To Layer Operator
 * \{ */

static int grease_pencil_move_to_layer_exec(bContext *C, wmOperator *op)
{
  using namespace bke::greasepencil;
  const Scene *scene = CTX_data_scene(C);
  bool changed = false;

  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  int target_layer_name_length;
  char *target_layer_name = RNA_string_get_alloc(
      op->ptr, "target_layer_name", nullptr, 0, &target_layer_name_length);
  BLI_SCOPED_DEFER([&] { MEM_SAFE_FREE(target_layer_name); });
  const bool add_new_layer = RNA_boolean_get(op->ptr, "add_new_layer");
  if (add_new_layer) {
    grease_pencil.add_layer(target_layer_name);
  }

  TreeNode *target_node = grease_pencil.find_node_by_name(target_layer_name);
  if (target_node == nullptr || !target_node->is_layer()) {
    BKE_reportf(op->reports, RPT_ERROR, "There is no layer '%s'", target_layer_name);
    return OPERATOR_CANCELLED;
  }

  Layer &layer_dst = target_node->as_layer();
  if (layer_dst.is_locked()) {
    BKE_reportf(op->reports, RPT_ERROR, "'%s' Layer is locked", target_layer_name);
    return OPERATOR_CANCELLED;
  }

  /* Iterate through all the drawings at current scene frame. */
  const Vector<MutableDrawingInfo> drawings_src = retrieve_editable_drawings(*scene,
                                                                             grease_pencil);
  for (const MutableDrawingInfo &info : drawings_src) {
    bke::CurvesGeometry &curves_src = info.drawing.strokes_for_write();
    IndexMaskMemory memory;
    const IndexMask selected_strokes = ed::curves::retrieve_selected_curves(curves_src, memory);
    if (selected_strokes.is_empty()) {
      continue;
    }

    if (!layer_dst.has_drawing_at(info.frame_number)) {
      /* Move geometry to a new drawing in target layer. */
      Drawing &drawing_dst = *grease_pencil.insert_frame(layer_dst, info.frame_number);
      drawing_dst.strokes_for_write() = bke::curves_copy_curve_selection(
          curves_src, selected_strokes, {});

      curves_src.remove_curves(selected_strokes, {});
      drawing_dst.tag_topology_changed();
    }
    else if (Drawing *drawing_dst = grease_pencil.get_editable_drawing_at(layer_dst,
                                                                          info.frame_number))
    {
      /* Append geometry to drawing in target layer. */
      bke::CurvesGeometry selected_elems = curves_copy_curve_selection(
          curves_src, selected_strokes, {});
      Curves *selected_curves = bke::curves_new_nomain(std::move(selected_elems));
      Curves *layer_curves = bke::curves_new_nomain(std::move(drawing_dst->strokes_for_write()));
      std::array<bke::GeometrySet, 2> geometry_sets{bke::GeometrySet::from_curves(selected_curves),
                                                    bke::GeometrySet::from_curves(layer_curves)};
      bke::GeometrySet joined = geometry::join_geometries(geometry_sets, {});
      drawing_dst->strokes_for_write() = std::move(joined.get_curves_for_write()->geometry.wrap());

      curves_src.remove_curves(selected_strokes, {});

      drawing_dst->tag_topology_changed();
    }

    info.drawing.tag_topology_changed();
    changed = true;
  }

  if (changed) {
    /* updates */
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static int grease_pencil_move_to_layer_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  const bool add_new_layer = RNA_boolean_get(op->ptr, "add_new_layer");
  if (add_new_layer) {
    return WM_operator_props_popup_confirm_ex(
        C, op, event, IFACE_("Move to New Layer"), IFACE_("Create"));
  }
  return grease_pencil_move_to_layer_exec(C, op);
}

static void GREASE_PENCIL_OT_move_to_layer(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers. */
  ot->name = "Move to Layer";
  ot->idname = "GREASE_PENCIL_OT_move_to_layer";
  ot->description = "Move selected strokes to another layer";

  /* callbacks. */
  ot->invoke = grease_pencil_move_to_layer_invoke;
  ot->exec = grease_pencil_move_to_layer_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_string(
      ot->srna, "target_layer_name", "Layer", INT16_MAX, "Name", "Target Grease Pencil Layer");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  prop = RNA_def_boolean(
      ot->srna, "add_new_layer", false, "New Layer", "Move selection to a new layer");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Separate Operator
 * \{ */

enum class SeparateMode : int8_t {
  /* Selected Points/Strokes. */
  SELECTED = 0,
  /* By Material. */
  MATERIAL = 1,
  /* By Active Layer. */
  LAYER = 2,
};

static const EnumPropertyItem prop_separate_modes[] = {
    {int(SeparateMode::SELECTED), "SELECTED", 0, "Selection", "Separate selected geometry"},
    {int(SeparateMode::MATERIAL), "MATERIAL", 0, "By Material", "Separate by material"},
    {int(SeparateMode::LAYER), "LAYER", 0, "By Layer", "Separate by layer"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void remove_unused_materials(Main *bmain, Object *object)
{
  int actcol = object->actcol;
  for (int slot = 1; slot <= object->totcol; slot++) {
    while (slot <= object->totcol && !BKE_object_material_slot_used(object, slot)) {
      object->actcol = slot;
      if (!BKE_object_material_slot_remove(bmain, object)) {
        break;
      }

      if (actcol >= slot) {
        actcol--;
      }
    }
  }
  object->actcol = actcol;
}

static Object *duplicate_grease_pencil_object(Main *bmain,
                                              Scene *scene,
                                              ViewLayer *view_layer,
                                              Base *base_prev,
                                              const GreasePencil &grease_pencil_src)
{
  const eDupli_ID_Flags dupflag = eDupli_ID_Flags(U.dupflag & USER_DUP_ACT);
  Base *base_new = object::add_duplicate(bmain, scene, view_layer, base_prev, dupflag);
  Object *object_dst = base_new->object;
  object_dst->mode = OB_MODE_OBJECT;
  object_dst->data = BKE_grease_pencil_add(bmain, grease_pencil_src.id.name + 2);

  return object_dst;
}

static bke::greasepencil::Layer &find_or_create_layer_in_dst_by_name(
    const int layer_index, const GreasePencil &grease_pencil_src, GreasePencil &grease_pencil_dst)
{
  using namespace bke::greasepencil;

  /* This assumes that the index is valid. Will cause an assert if it is not. */
  const Layer &layer_src = *grease_pencil_src.layer(layer_index);
  if (TreeNode *node = grease_pencil_dst.find_node_by_name(layer_src.name())) {
    return node->as_layer();
  }

  /* If the layer can't be found in `grease_pencil_dst` by name add a new layer. */
  Layer &new_layer = grease_pencil_dst.add_layer(layer_src.name());

  /* Transfer Layer attributes. */
  bke::gather_attributes(grease_pencil_src.attributes(),
                         bke::AttrDomain::Layer,
                         {},
                         {},
                         Span({layer_index}),
                         grease_pencil_dst.attributes_for_write());

  return new_layer;
}

static bool grease_pencil_separate_selected(bContext &C,
                                            Main &bmain,
                                            Scene &scene,
                                            ViewLayer &view_layer,
                                            Base &base_prev,
                                            Object &object_src)
{
  using namespace bke::greasepencil;
  bool changed = false;

  GreasePencil &grease_pencil_src = *static_cast<GreasePencil *>(object_src.data);
  Object *object_dst = duplicate_grease_pencil_object(
      &bmain, &scene, &view_layer, &base_prev, grease_pencil_src);
  GreasePencil &grease_pencil_dst = *static_cast<GreasePencil *>(object_dst->data);

  /* Iterate through all the drawings at current scene frame. */
  const Vector<MutableDrawingInfo> drawings_src = retrieve_editable_drawings(scene,
                                                                             grease_pencil_src);
  for (const MutableDrawingInfo &info : drawings_src) {
    bke::CurvesGeometry &curves_src = info.drawing.strokes_for_write();
    IndexMaskMemory memory;
    const IndexMask selected_points = ed::curves::retrieve_selected_points(curves_src, memory);
    if (selected_points.is_empty()) {
      continue;
    }

    /* Insert Keyframe at current frame/layer. */
    Layer &layer_dst = find_or_create_layer_in_dst_by_name(
        info.layer_index, grease_pencil_src, grease_pencil_dst);

    Drawing *drawing_dst = grease_pencil_dst.insert_frame(layer_dst, info.frame_number);
    /* TODO: Can we assume the insert never fails? */
    BLI_assert(drawing_dst != nullptr);

    /* Copy strokes to new CurvesGeometry. */
    drawing_dst->strokes_for_write() = bke::curves_copy_point_selection(
        curves_src, selected_points, {});
    curves_src = remove_points_and_split(curves_src, selected_points);

    info.drawing.tag_topology_changed();
    drawing_dst->tag_topology_changed();

    changed = true;
  }

  if (changed) {
    grease_pencil_dst.set_active_layer(nullptr);

    /* Add object materials to target object. */
    BKE_object_material_array_assign(&bmain,
                                     object_dst,
                                     BKE_object_material_array_p(&object_src),
                                     *BKE_object_material_len_p(&object_src),
                                     false);

    remove_unused_materials(&bmain, object_dst);
    DEG_id_tag_update(&grease_pencil_dst.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, &grease_pencil_dst);
  }
  return changed;
}

static bool grease_pencil_separate_layer(bContext &C,
                                         Main &bmain,
                                         Scene &scene,
                                         ViewLayer &view_layer,
                                         Base &base_prev,
                                         Object &object_src)
{
  using namespace bke::greasepencil;
  bool changed = false;

  GreasePencil &grease_pencil_src = *static_cast<GreasePencil *>(object_src.data);

  /* Create a new object for each layer. */
  for (const int layer_i : grease_pencil_src.layers().index_range()) {
    Layer *layer_src = grease_pencil_src.layer(layer_i);
    if (layer_src->is_selected() || layer_src->is_locked()) {
      continue;
    }

    Object *object_dst = duplicate_grease_pencil_object(
        &bmain, &scene, &view_layer, &base_prev, grease_pencil_src);
    GreasePencil &grease_pencil_dst = *static_cast<GreasePencil *>(object_dst->data);
    Layer &layer_dst = find_or_create_layer_in_dst_by_name(
        layer_i, grease_pencil_src, grease_pencil_dst);

    /* Iterate through all the drawings at current frame. */
    const Vector<MutableDrawingInfo> drawings_src = retrieve_editable_drawings_from_layer(
        scene, grease_pencil_src, *layer_src);
    for (const MutableDrawingInfo &info : drawings_src) {
      bke::CurvesGeometry &curves_src = info.drawing.strokes_for_write();
      IndexMaskMemory memory;
      const IndexMask strokes = retrieve_editable_strokes(
          object_src, info.drawing, info.layer_index, memory);
      if (strokes.is_empty()) {
        continue;
      }

      /* Add object materials. */
      BKE_object_material_array_assign(&bmain,
                                       object_dst,
                                       BKE_object_material_array_p(&object_src),
                                       *BKE_object_material_len_p(&object_src),
                                       false);

      /* Insert Keyframe at current frame/layer. */
      Drawing *drawing_dst = grease_pencil_dst.insert_frame(layer_dst, info.frame_number);
      /* TODO: Can we assume the insert never fails? */
      BLI_assert(drawing_dst != nullptr);

      /* Copy strokes to new CurvesGeometry. */
      drawing_dst->strokes_for_write() = bke::curves_copy_curve_selection(
          info.drawing.strokes(), strokes, {});
      curves_src.remove_curves(strokes, {});

      info.drawing.tag_topology_changed();
      drawing_dst->tag_topology_changed();

      changed = true;
    }

    remove_unused_materials(&bmain, object_dst);

    DEG_id_tag_update(&grease_pencil_dst.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, &grease_pencil_dst);
  }

  return changed;
}

static bool grease_pencil_separate_material(bContext &C,
                                            Main &bmain,
                                            Scene &scene,
                                            ViewLayer &view_layer,
                                            Base &base_prev,
                                            Object &object_src)
{
  using namespace blender::bke;
  using namespace bke::greasepencil;
  bool changed = false;

  GreasePencil &grease_pencil_src = *static_cast<GreasePencil *>(object_src.data);

  /* Create a new object for each material. */
  for (const int mat_i : IndexRange(object_src.totcol).drop_front(1)) {
    if (!BKE_object_material_slot_used(&object_src, mat_i + 1)) {
      continue;
    }

    Object *object_dst = duplicate_grease_pencil_object(
        &bmain, &scene, &view_layer, &base_prev, grease_pencil_src);

    /* Add object materials. */
    BKE_object_material_array_assign(&bmain,
                                     object_dst,
                                     BKE_object_material_array_p(&object_src),
                                     *BKE_object_material_len_p(&object_src),
                                     false);

    /* Iterate through all the drawings at current scene frame. */
    const Vector<MutableDrawingInfo> drawings_src = retrieve_editable_drawings(scene,
                                                                               grease_pencil_src);
    for (const MutableDrawingInfo &info : drawings_src) {
      bke::CurvesGeometry &curves_src = info.drawing.strokes_for_write();
      IndexMaskMemory memory;
      const IndexMask strokes = retrieve_editable_strokes_by_material(
          object_src, info.drawing, mat_i, memory);
      if (strokes.is_empty()) {
        continue;
      }

      GreasePencil &grease_pencil_dst = *static_cast<GreasePencil *>(object_dst->data);

      /* Insert Keyframe at current frame/layer. */
      Layer &layer_dst = find_or_create_layer_in_dst_by_name(
          info.layer_index, grease_pencil_src, grease_pencil_dst);

      Drawing *drawing_dst = grease_pencil_dst.insert_frame(layer_dst, info.frame_number);
      /* TODO: Can we assume the insert never fails? */
      BLI_assert(drawing_dst != nullptr);

      /* Copy strokes to new CurvesGeometry. */
      drawing_dst->strokes_for_write() = bke::curves_copy_curve_selection(curves_src, strokes, {});
      curves_src.remove_curves(strokes, {});

      info.drawing.tag_topology_changed();
      drawing_dst->tag_topology_changed();
      DEG_id_tag_update(&grease_pencil_dst.id, ID_RECALC_GEOMETRY);
      WM_event_add_notifier(&C, NC_OBJECT | ND_DRAW, &grease_pencil_dst);

      changed = true;
    }

    remove_unused_materials(&bmain, object_dst);
  }

  if (changed) {
    remove_unused_materials(&bmain, &object_src);
  }

  return changed;
}

static int grease_pencil_separate_exec(bContext *C, wmOperator *op)
{
  using namespace bke::greasepencil;
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Base *base_prev = CTX_data_active_base(C);
  Object *object_src = CTX_data_active_object(C);
  GreasePencil &grease_pencil_src = *static_cast<GreasePencil *>(object_src->data);

  const SeparateMode mode = SeparateMode(RNA_enum_get(op->ptr, "mode"));
  bool changed = false;

  WM_cursor_wait(true);

  switch (mode) {
    case SeparateMode::SELECTED: {
      /* Cancel if nothing selected. */
      const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene,
                                                                             grease_pencil_src);
      const bool has_selection = std::any_of(
          drawings.begin(), drawings.end(), [&](const MutableDrawingInfo &info) {
            return ed::curves::has_anything_selected(info.drawing.strokes());
          });
      if (!has_selection) {
        BKE_report(op->reports, RPT_ERROR, "Nothing selected");
        WM_cursor_wait(false);
        return OPERATOR_CANCELLED;
      }

      changed = grease_pencil_separate_selected(
          *C, *bmain, *scene, *view_layer, *base_prev, *object_src);
      break;
    }
    case SeparateMode::MATERIAL: {
      /* Cancel if the object only has one material. */
      if (object_src->totcol == 1) {
        BKE_report(op->reports, RPT_ERROR, "The object has only one material");
        WM_cursor_wait(false);
        return OPERATOR_CANCELLED;
      }

      changed = grease_pencil_separate_material(
          *C, *bmain, *scene, *view_layer, *base_prev, *object_src);
      break;
    }
    case SeparateMode::LAYER: {
      /* Cancel if the object only has one layer. */
      if (grease_pencil_src.layers().size() == 1) {
        BKE_report(op->reports, RPT_ERROR, "The object has only one layer");
        WM_cursor_wait(false);
        return OPERATOR_CANCELLED;
      }
      changed = grease_pencil_separate_layer(
          *C, *bmain, *scene, *view_layer, *base_prev, *object_src);
      break;
    }
  }

  WM_cursor_wait(false);

  if (changed) {
    DEG_id_tag_update(&grease_pencil_src.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA | NA_EDITED, &grease_pencil_src);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_separate(wmOperatorType *ot)
{
  /* identifiers. */
  ot->name = "Separate";
  ot->idname = "GREASE_PENCIL_OT_separate";
  ot->description = "Separate the selected geometry into a new grease pencil object";

  /* callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_separate_exec;
  ot->poll = editable_grease_pencil_poll;

  /* flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties. */
  ot->prop = RNA_def_enum(
      ot->srna, "mode", prop_separate_modes, int(SeparateMode::SELECTED), "Mode", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy and Paste Operator
 * \{ */

/* Global clipboard for Grease Pencil curves. */
static struct Clipboard {
  bke::CurvesGeometry curves;
  /* We store the material uid's of the copied curves, so we can match those when pasting the
   * clipboard into another object. */
  Vector<std::pair<uint, int>> materials;
  int materials_in_source_num;
} *grease_pencil_clipboard = nullptr;

static Clipboard &ensure_grease_pencil_clipboard()
{
  if (grease_pencil_clipboard == nullptr) {
    grease_pencil_clipboard = MEM_new<Clipboard>(__func__);
  }
  return *grease_pencil_clipboard;
}

static int grease_pencil_paste_strokes_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  const Scene &scene = *CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene.toolsettings);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const bool paste_on_back = RNA_boolean_get(op->ptr, "paste_back");

  /* Get active layer in the target object. */
  if (!grease_pencil.has_active_layer()) {
    BKE_report(op->reports, RPT_ERROR, "No active Grease Pencil layer");
    return OPERATOR_CANCELLED;
  }
  const bke::greasepencil::Layer &active_layer = *grease_pencil.get_active_layer();
  if (!active_layer.is_editable()) {
    BKE_report(op->reports, RPT_ERROR, "Active layer is locked or hidden");
    return OPERATOR_CANCELLED;
  }

  /* Ensure active keyframe. */
  bool inserted_keyframe = false;
  if (!ensure_active_keyframe(scene, grease_pencil, inserted_keyframe)) {
    BKE_report(op->reports, RPT_ERROR, "No Grease Pencil frame to draw on");
    return OPERATOR_CANCELLED;
  }
  bke::greasepencil::Drawing *target_drawing = grease_pencil.get_editable_drawing_at(active_layer,
                                                                                     scene.r.cfra);
  if (target_drawing == nullptr) {
    return OPERATOR_CANCELLED;
  }

  /* Deselect everything from editable drawings. The pasted strokes are the only ones then after
   * the paste. That's convenient for the user. */
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::GSpanAttributeWriter selection_in_target = ed::curves::ensure_selection_attribute(
        info.drawing.strokes_for_write(), selection_domain, CD_PROP_BOOL);
    ed::curves::fill_selection_false(selection_in_target.span);
    selection_in_target.finish();
  });

  clipboard_paste_strokes(*bmain, *object, *target_drawing, paste_on_back);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  if (inserted_keyframe) {
    WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static int grease_pencil_copy_strokes_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  const Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const bke::AttrDomain selection_domain = ED_grease_pencil_selection_domain_get(
      scene->toolsettings);

  Clipboard &clipboard = ensure_grease_pencil_clipboard();

  bool anything_copied = false;
  int num_copied = 0;
  Vector<bke::GeometrySet> set_of_copied_curves;

  /* Collect all selected strokes/points on all editable layers. */
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  for (const MutableDrawingInfo &drawing_info : drawings) {
    const bke::CurvesGeometry &curves = drawing_info.drawing.strokes();

    if (curves.curves_num() == 0) {
      continue;
    }
    if (!ed::curves::has_anything_selected(curves)) {
      continue;
    }

    /* Get a copy of the selected geometry on this layer. */
    IndexMaskMemory memory;
    bke::CurvesGeometry copied_curves;

    if (selection_domain == bke::AttrDomain::Curve) {
      const IndexMask selected_curves = ed::curves::retrieve_selected_curves(curves, memory);
      copied_curves = curves_copy_curve_selection(curves, selected_curves, {});
      num_copied += copied_curves.curves_num();
    }
    else if (selection_domain == bke::AttrDomain::Point) {
      const IndexMask selected_points = ed::curves::retrieve_selected_points(curves, memory);
      copied_curves = curves_copy_point_selection(curves, selected_points, {});
      num_copied += copied_curves.points_num();
    }

    /* Add the layer selection to the set of copied curves. */
    Curves *layer_curves = curves_new_nomain(std::move(copied_curves));
    set_of_copied_curves.append(bke::GeometrySet::from_curves(layer_curves));
    anything_copied = true;
  }

  if (!anything_copied) {
    clipboard.curves.resize(0, 0);
    return OPERATOR_CANCELLED;
  }

  /* Merge all copied curves into one CurvesGeometry object and assign it to the clipboard. */
  bke::GeometrySet joined_copied_curves = geometry::join_geometries(set_of_copied_curves, {});
  clipboard.curves = std::move(joined_copied_curves.get_curves_for_write()->geometry.wrap());

  /* Store the session uid of the materials used by the curves in the clipboard. We use the uid to
   * remap the material indices when pasting. */
  clipboard.materials.clear();
  clipboard.materials_in_source_num = grease_pencil.material_array_num;
  const bke::AttributeAccessor attributes = clipboard.curves.attributes();
  const VArraySpan<int> material_indices = *attributes.lookup_or_default<int>(
      "material_index", bke::AttrDomain::Curve, 0);
  for (const int material_index : IndexRange(grease_pencil.material_array_num)) {
    if (!material_indices.contains(material_index)) {
      continue;
    }
    const Material *material = grease_pencil.material_array[material_index];
    clipboard.materials.append({material->id.session_uid, material_index});
  }

  /* Report the numbers. */
  if (selection_domain == bke::AttrDomain::Curve) {
    BKE_reportf(op->reports, RPT_INFO, "Copied %d selected curve(s)", num_copied);
  }
  else if (selection_domain == bke::AttrDomain::Point) {
    BKE_reportf(op->reports, RPT_INFO, "Copied %d selected point(s)", num_copied);
  }

  return OPERATOR_FINISHED;
}

static bool grease_pencil_paste_strokes_poll(bContext *C)
{
  if (!editable_grease_pencil_poll(C)) {
    return false;
  }

  /* Check for curves in the Grease Pencil clipboard. */
  return (grease_pencil_clipboard && grease_pencil_clipboard->curves.curves_num() > 0);
}

static void GREASE_PENCIL_OT_paste(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Paste Strokes";
  ot->idname = "GREASE_PENCIL_OT_paste";
  ot->description =
      "Paste Grease Pencil points or strokes from the internal clipboard to the active layer";

  /* Callbacks. */
  ot->exec = grease_pencil_paste_strokes_exec;
  ot->poll = grease_pencil_paste_strokes_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_boolean(
      ot->srna, "paste_back", false, "Paste on Back", "Add pasted strokes behind all strokes");
  RNA_def_property_flag(ot->prop, PROP_SKIP_SAVE);
}

static void GREASE_PENCIL_OT_copy(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Copy Strokes";
  ot->idname = "GREASE_PENCIL_OT_copy";
  ot->description = "Copy the selected Grease Pencil points or strokes to the internal clipboard";

  /* Callbacks. */
  ot->exec = grease_pencil_copy_strokes_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

void clipboard_free()
{
  if (grease_pencil_clipboard) {
    MEM_delete(grease_pencil_clipboard);
    grease_pencil_clipboard = nullptr;
  }
}

const bke::CurvesGeometry &clipboard_curves()
{
  using namespace blender::ed::greasepencil;
  return ensure_grease_pencil_clipboard().curves;
}

static Array<int> clipboard_materials_remap(Main &bmain, Object &object)
{
  using namespace blender::ed::greasepencil;

  /* Get a list of all materials in the scene. */
  Map<uint, Material *> scene_materials;
  LISTBASE_FOREACH (Material *, material, &bmain.materials) {
    scene_materials.add(material->id.session_uid, material);
  }

  Clipboard &clipboard = ensure_grease_pencil_clipboard();
  Array<int> clipboard_material_remap(clipboard.materials_in_source_num, 0);
  for (const int i : clipboard.materials.index_range()) {
    /* Check if the material name exists in the scene. */
    int target_index;
    uint material_id = clipboard.materials[i].first;
    Material *material = scene_materials.lookup_default(material_id, nullptr);
    if (!material) {
      /* Material is removed, so create a new material. */
      BKE_grease_pencil_object_material_new(&bmain, &object, nullptr, &target_index);
      clipboard_material_remap[clipboard.materials[i].second] = target_index;
      continue;
    }

    /* Find or add the material to the target object. */
    target_index = BKE_object_material_ensure(&bmain, &object, material);
    clipboard_material_remap[clipboard.materials[i].second] = target_index;
  }

  return clipboard_material_remap;
}

IndexRange clipboard_paste_strokes(Main &bmain,
                                   Object &object,
                                   bke::greasepencil::Drawing &drawing,
                                   const bool paste_back)
{
  const bke::CurvesGeometry &clipboard_curves = ed::greasepencil::clipboard_curves();

  /* Get a list of all materials in the scene. */
  const Array<int> clipboard_material_remap = ed::greasepencil::clipboard_materials_remap(bmain,
                                                                                          object);

  /* Get the index range of the pasted curves in the target layer. */
  const IndexRange pasted_curves_range = paste_back ?
                                             IndexRange(0, clipboard_curves.curves_num()) :
                                             IndexRange(drawing.strokes().curves_num(),
                                                        clipboard_curves.curves_num());

  /* Append the geometry from the clipboard to the target layer. */
  Curves *clipboard_id = bke::curves_new_nomain(clipboard_curves);
  Curves *target_id = curves_new_nomain(std::move(drawing.strokes_for_write()));
  const Array<bke::GeometrySet> geometry_sets = {
      bke::GeometrySet::from_curves(paste_back ? clipboard_id : target_id),
      bke::GeometrySet::from_curves(paste_back ? target_id : clipboard_id)};
  bke::GeometrySet joined_curves = geometry::join_geometries(geometry_sets, {});
  drawing.strokes_for_write() = std::move(joined_curves.get_curves_for_write()->geometry.wrap());

  /* Remap the material indices of the pasted curves to the target object material indices. */
  bke::MutableAttributeAccessor attributes = drawing.strokes_for_write().attributes_for_write();
  bke::SpanAttributeWriter<int> material_indices = attributes.lookup_or_add_for_write_span<int>(
      "material_index", bke::AttrDomain::Curve);
  if (material_indices) {
    for (const int i : pasted_curves_range) {
      material_indices.span[i] = clipboard_material_remap[material_indices.span[i]];
    }
    material_indices.finish();
  }

  drawing.tag_topology_changed();

  return pasted_curves_range;
}

/* -------------------------------------------------------------------- */
/** \name Merge Stroke Operator
 * \{ */
static int grease_pencil_stroke_merge_by_distance_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const float threshold = RNA_float_get(op->ptr, "threshold");
  const bool use_unselected = RNA_boolean_get(op->ptr, "use_unselected");

  std::atomic<bool> changed = false;

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::greasepencil::Drawing &drawing = info.drawing;
    IndexMaskMemory memory;
    const IndexMask points = use_unselected ?
                                 ed::greasepencil::retrieve_editable_points(
                                     *object, drawing, info.layer_index, memory) :
                                 ed::greasepencil::retrieve_editable_and_selected_points(
                                     *object, info.drawing, info.layer_index, memory);
    if (points.is_empty()) {
      return;
    }
    drawing.strokes_for_write() = curves_merge_by_distance(
        drawing.strokes(), threshold, points, {});
    drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });
  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }
  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_stroke_merge_by_distance(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers. */
  ot->name = "Merge by Distance";
  ot->idname = "GREASE_PENCIL_OT_stroke_merge_by_distance";
  ot->description = "Merge points by distance";

  /* Callbacks. */
  ot->exec = grease_pencil_stroke_merge_by_distance_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Merge parameters. */
  prop = RNA_def_float(ot->srna, "threshold", 0.001f, 0.0f, 100.0f, "Threshold", "", 0.0f, 100.0f);
  /* Avoid re-using last var. */
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  prop = RNA_def_boolean(ot->srna,
                         "use_unselected",
                         false,
                         "Unselected",
                         "Use whole stroke, not only selected points");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Extrude Operator
 * \{ */

static bke::CurvesGeometry extrude_grease_pencil_curves(const bke::CurvesGeometry &src,
                                                        const IndexMask &points_to_extrude)
{
  const OffsetIndices<int> points_by_curve = src.points_by_curve();

  const int old_curves_num = src.curves_num();
  const int old_points_num = src.points_num();

  Vector<int> dst_to_src_points(old_points_num);
  array_utils::fill_index_range(dst_to_src_points.as_mutable_span());

  Vector<int> dst_to_src_curves(old_curves_num);
  array_utils::fill_index_range(dst_to_src_curves.as_mutable_span());

  Vector<bool> dst_selected(old_points_num, false);

  Vector<int> dst_curve_counts(old_curves_num);
  offset_indices::copy_group_sizes(
      points_by_curve, src.curves_range(), dst_curve_counts.as_mutable_span());

  const VArray<bool> &src_cyclic = src.cyclic();

  /* Point offset keeps track of the points inserted. */
  int point_offset = 0;
  for (const int curve_index : src.curves_range()) {
    const IndexRange curve_points = points_by_curve[curve_index];
    const IndexMask curve_points_to_extrude = points_to_extrude.slice_content(curve_points);
    const bool curve_cyclic = src_cyclic[curve_index];

    curve_points_to_extrude.foreach_index([&](const int src_point_index) {
      if (!curve_cyclic && (src_point_index == curve_points.first())) {
        /* Start-point extruded, we insert a new point at the beginning of the curve.
         * NOTE: all points of a cyclic curve behave like an inner-point. */
        dst_to_src_points.insert(src_point_index + point_offset, src_point_index);
        dst_selected.insert(src_point_index + point_offset, true);
        ++dst_curve_counts[curve_index];
        ++point_offset;
        return;
      }
      if (!curve_cyclic && (src_point_index == curve_points.last())) {
        /* End-point extruded, we insert a new point at the end of the curve.
         * NOTE: all points of a cyclic curve behave like an inner-point. */
        dst_to_src_points.insert(src_point_index + point_offset + 1, src_point_index);
        dst_selected.insert(src_point_index + point_offset + 1, true);
        ++dst_curve_counts[curve_index];
        ++point_offset;
        return;
      }

      /* Inner-point extruded: we create a new curve made of two points located at the same
       * position. Only one of them is selected so that the other one remains stuck to the curve.
       */
      dst_to_src_points.append(src_point_index);
      dst_selected.append(false);
      dst_to_src_points.append(src_point_index);
      dst_selected.append(true);
      dst_to_src_curves.append(curve_index);
      dst_curve_counts.append(2);
    });
  }

  const int new_points_num = dst_to_src_points.size();
  const int new_curves_num = dst_to_src_curves.size();

  bke::CurvesGeometry dst(new_points_num, new_curves_num);

  /* Setup curve offsets, based on the number of points in each curve. */
  MutableSpan<int> new_curve_offsets = dst.offsets_for_write();
  array_utils::copy(dst_curve_counts.as_span(), new_curve_offsets.drop_back(1));
  offset_indices::accumulate_counts_to_offsets(new_curve_offsets);

  /* Attributes. */
  const bke::AttributeAccessor src_attributes = src.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  bke::gather_attributes(
      src_attributes, bke::AttrDomain::Curve, {}, {}, dst_to_src_curves, dst_attributes);

  bke::gather_attributes(
      src_attributes, bke::AttrDomain::Point, {}, {}, dst_to_src_points, dst_attributes);

  /* Selection attribute. */
  const std::string &selection_attr_name = ".selection";
  bke::SpanAttributeWriter<bool> selection =
      dst_attributes.lookup_or_add_for_write_only_span<bool>(selection_attr_name,
                                                             bke::AttrDomain::Point);
  array_utils::copy(dst_selected.as_span(), selection.span);
  selection.finish();

  /* Cyclic attribute : newly created curves cannot be cyclic.
   * NOTE: if the cyclic attribute is single and false, it can be kept this way.
   */
  if (src_cyclic.get_if_single().value_or(true)) {
    dst.cyclic_for_write().drop_front(old_curves_num).fill(false);
  }

  dst.update_curve_types();
  return dst;
}

static int grease_pencil_extrude_exec(bContext *C, wmOperator * /*op*/)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  std::atomic<bool> changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    IndexMaskMemory memory;
    const IndexMask points_to_extrude = retrieve_editable_and_selected_points(
        *object, info.drawing, info.layer_index, memory);
    if (points_to_extrude.is_empty()) {
      return;
    }

    const bke::CurvesGeometry &curves = info.drawing.strokes();
    info.drawing.strokes_for_write() = extrude_grease_pencil_curves(curves, points_to_extrude);

    info.drawing.tag_topology_changed();
    changed.store(true, std::memory_order_relaxed);
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_extrude(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Extrude Stroke Points";
  ot->idname = "GREASE_PENCIL_OT_extrude";
  ot->description = "Extrude the selected points";

  /* Callbacks. */
  ot->exec = grease_pencil_extrude_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snapping Selection to Grid Operator
 * \{ */

/* Poll callback for snap operators */
/* NOTE: For now, we only allow these in the 3D view, as other editors do not
 *       define a cursor or grid-step which can be used.
 */
static bool grease_pencil_snap_poll(bContext *C)
{
  if (!editable_grease_pencil_poll(C)) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  return (area != nullptr) && (area->spacetype == SPACE_VIEW3D);
}

static int grease_pencil_snap_to_grid_exec(bContext *C, wmOperator * /*op*/)
{
  using bke::greasepencil::Layer;

  const Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object.data);
  const View3D &v3d = *CTX_wm_view3d(C);
  const ARegion &region = *CTX_wm_region(C);
  const float grid_size = ED_view3d_grid_view_scale(&scene, &v3d, &region, nullptr);

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  for (const MutableDrawingInfo &drawing_info : drawings) {
    bke::CurvesGeometry &curves = drawing_info.drawing.strokes_for_write();
    if (curves.curves_num() == 0) {
      continue;
    }
    if (!ed::curves::has_anything_selected(curves)) {
      continue;
    }

    IndexMaskMemory memory;
    const IndexMask selected_points = ed::curves::retrieve_selected_points(curves, memory);

    const Layer &layer = *grease_pencil.layer(drawing_info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(object);
    const float4x4 world_to_layer = math::invert(layer_to_world);

    MutableSpan<float3> positions = curves.positions_for_write();
    selected_points.foreach_index(GrainSize(4096), [&](const int point_i) {
      const float3 pos_world = math::transform_point(layer_to_world, positions[point_i]);
      const float3 pos_snapped = grid_size * math::floor(pos_world / grid_size + 0.5f);
      positions[point_i] = math::transform_point(world_to_layer, pos_snapped);
    });

    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    DEG_id_tag_update(&object.id, ID_RECALC_SYNC_TO_EVAL);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_snap_to_grid(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Selection to Grid";
  ot->idname = "GREASE_PENCIL_OT_snap_to_grid";
  ot->description = "Snap selected points to the nearest grid points";

  /* callbacks */
  ot->exec = grease_pencil_snap_to_grid_exec;
  ot->poll = grease_pencil_snap_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snapping Selection to Cursor Operator
 * \{ */

static int grease_pencil_snap_to_cursor_exec(bContext *C, wmOperator *op)
{
  using bke::greasepencil::Layer;

  const Scene &scene = *CTX_data_scene(C);
  Object &object = *CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object.data);
  const bool use_offset = RNA_boolean_get(op->ptr, "use_offset");
  const float3 cursor_world = scene.cursor.location;

  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(scene, grease_pencil);
  for (const MutableDrawingInfo &drawing_info : drawings) {
    bke::CurvesGeometry &curves = drawing_info.drawing.strokes_for_write();
    if (curves.curves_num() == 0) {
      continue;
    }
    if (!ed::curves::has_anything_selected(curves)) {
      continue;
    }

    IndexMaskMemory selected_points_memory;
    const IndexMask selected_points = ed::curves::retrieve_selected_points(curves,
                                                                           selected_points_memory);

    const Layer &layer = *grease_pencil.layer(drawing_info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(object);
    const float4x4 world_to_layer = math::invert(layer_to_world);
    const float3 cursor_layer = math::transform_point(world_to_layer, cursor_world);

    MutableSpan<float3> positions = curves.positions_for_write();
    if (use_offset) {
      const OffsetIndices points_by_curve = curves.points_by_curve();
      IndexMaskMemory selected_curves_memory;
      const IndexMask selected_curves = ed::curves::retrieve_selected_curves(
          curves, selected_curves_memory);

      selected_curves.foreach_index(GrainSize(512), [&](const int curve_i) {
        const IndexRange points = points_by_curve[curve_i];

        /* Offset from first point of the curve. */
        const float3 offset = cursor_layer - positions[points.first()];
        selected_points.slice_content(points).foreach_index(
            GrainSize(4096), [&](const int point_i) { positions[point_i] += offset; });
      });
    }
    else {
      /* Set all selected positions to the cursor location. */
      index_mask::masked_fill(positions, cursor_layer, selected_points);
    }

    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    DEG_id_tag_update(&object.id, ID_RECALC_SYNC_TO_EVAL);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, nullptr);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_snap_to_cursor(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Selection to Cursor";
  ot->idname = "GREASE_PENCIL_OT_snap_to_cursor";
  ot->description = "Snap selected points/strokes to the cursor";

  /* callbacks */
  ot->exec = grease_pencil_snap_to_cursor_exec;
  ot->poll = grease_pencil_snap_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  ot->prop = RNA_def_boolean(ot->srna,
                             "use_offset",
                             true,
                             "With Offset",
                             "Offset the entire stroke instead of selected points only");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snapping Cursor to Selection Operator
 * \{ */

static bool grease_pencil_snap_compute_centroid(const Scene &scene,
                                                const Object &object,
                                                const GreasePencil &grease_pencil,
                                                float3 &r_centroid,
                                                float3 &r_min,
                                                float3 &r_max)
{
  using bke::greasepencil::Layer;

  int num_selected = 0;
  r_centroid = float3(0.0f);
  r_min = float3(std::numeric_limits<float>::max());
  r_max = float3(std::numeric_limits<float>::lowest());

  const Vector<DrawingInfo> drawings = retrieve_visible_drawings(scene, grease_pencil, false);
  for (const DrawingInfo &drawing_info : drawings) {
    const bke::CurvesGeometry &curves = drawing_info.drawing.strokes();
    if (curves.curves_num() == 0) {
      continue;
    }
    if (!ed::curves::has_anything_selected(curves)) {
      continue;
    }

    IndexMaskMemory selected_points_memory;
    const IndexMask selected_points = ed::curves::retrieve_selected_points(curves,
                                                                           selected_points_memory);

    const Layer &layer = *grease_pencil.layer(drawing_info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(object);

    Span<float3> positions = curves.positions();
    selected_points.foreach_index(GrainSize(4096), [&](const int point_i) {
      const float3 pos_world = math::transform_point(layer_to_world, positions[point_i]);
      r_centroid += pos_world;
      math::min_max(pos_world, r_min, r_max);
    });
    num_selected += selected_points.size();
  }
  if (num_selected == 0) {
    r_min = r_max = float3(0.0f);
    return false;
  }

  r_centroid /= num_selected;
  return true;
}

static int grease_pencil_snap_cursor_to_sel_exec(bContext *C, wmOperator * /*op*/)
{
  Scene &scene = *CTX_data_scene(C);
  const Object &object = *CTX_data_active_object(C);
  const GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object.data);
  float3 &cursor = reinterpret_cast<float3 &>(scene.cursor.location);

  float3 centroid, points_min, points_max;
  if (!grease_pencil_snap_compute_centroid(
          scene, object, grease_pencil, centroid, points_min, points_max))
  {
    return OPERATOR_FINISHED;
  }

  switch (scene.toolsettings->transform_pivot_point) {
    case V3D_AROUND_CENTER_BOUNDS:
      cursor = math::midpoint(points_min, points_max);
      break;
    case V3D_AROUND_CENTER_MEDIAN:
    case V3D_AROUND_CURSOR:
    case V3D_AROUND_LOCAL_ORIGINS:
    case V3D_AROUND_ACTIVE:
      cursor = centroid;
      break;
    default:
      BLI_assert_unreachable();
  }

  DEG_id_tag_update(&scene.id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_VIEW3D, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_snap_cursor_to_selected(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Snap Cursor to Selected Points";
  ot->idname = "GREASE_PENCIL_OT_snap_cursor_to_selected";
  ot->description = "Snap cursor to center of selected points";

  /* callbacks */
  ot->exec = grease_pencil_snap_cursor_to_sel_exec;
  ot->poll = grease_pencil_snap_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Curve Type Operator
 * \{ */

static int grease_pencil_set_curve_type_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);

  const CurveType dst_type = CurveType(RNA_enum_get(op->ptr, "type"));
  const bool use_handles = RNA_boolean_get(op->ptr, "use_handles");

  bool changed = false;
  const Vector<MutableDrawingInfo> drawings = retrieve_editable_drawings(*scene, grease_pencil);
  threading::parallel_for_each(drawings, [&](const MutableDrawingInfo &info) {
    bke::CurvesGeometry &curves = info.drawing.strokes_for_write();
    IndexMaskMemory memory;
    const IndexMask strokes = ed::greasepencil::retrieve_editable_and_selected_strokes(
        *object, info.drawing, info.layer_index, memory);
    if (strokes.is_empty()) {
      return;
    }

    geometry::ConvertCurvesOptions options;
    options.convert_bezier_handles_to_poly_points = use_handles;
    options.convert_bezier_handles_to_catmull_rom_points = use_handles;
    options.keep_bezier_shape_as_nurbs = use_handles;
    options.keep_catmull_rom_shape_as_nurbs = use_handles;

    curves = geometry::convert_curves(curves, strokes, dst_type, {}, options);
    info.drawing.tag_topology_changed();

    changed = true;
  });

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_set_curve_type(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Curve Type";
  ot->idname = "GREASE_PENCIL_OT_set_curve_type";
  ot->description = "Set type of selected curves";

  /* Callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = grease_pencil_set_curve_type_exec;
  ot->poll = editable_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "type", rna_enum_curves_type_items, CURVE_TYPE_POLY, "Type", "Curve type");

  RNA_def_boolean(ot->srna,
                  "use_handles",
                  false,
                  "Handles",
                  "Take handle information into account in the conversion");
}

/** \} */

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_edit()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_smooth);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_simplify);
  WM_operatortype_append(GREASE_PENCIL_OT_delete);
  WM_operatortype_append(GREASE_PENCIL_OT_dissolve);
  WM_operatortype_append(GREASE_PENCIL_OT_delete_frame);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_material_set);
  WM_operatortype_append(GREASE_PENCIL_OT_cyclical_set);
  WM_operatortype_append(GREASE_PENCIL_OT_set_active_material);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_switch_direction);
  WM_operatortype_append(GREASE_PENCIL_OT_set_uniform_thickness);
  WM_operatortype_append(GREASE_PENCIL_OT_set_uniform_opacity);
  WM_operatortype_append(GREASE_PENCIL_OT_caps_set);
  WM_operatortype_append(GREASE_PENCIL_OT_duplicate);
  WM_operatortype_append(GREASE_PENCIL_OT_set_material);
  WM_operatortype_append(GREASE_PENCIL_OT_clean_loose);
  WM_operatortype_append(GREASE_PENCIL_OT_separate);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_subdivide);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_reorder);
  WM_operatortype_append(GREASE_PENCIL_OT_move_to_layer);
  WM_operatortype_append(GREASE_PENCIL_OT_copy);
  WM_operatortype_append(GREASE_PENCIL_OT_paste);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_merge_by_distance);
  WM_operatortype_append(GREASE_PENCIL_OT_stroke_cutter);
  WM_operatortype_append(GREASE_PENCIL_OT_extrude);
  WM_operatortype_append(GREASE_PENCIL_OT_snap_to_grid);
  WM_operatortype_append(GREASE_PENCIL_OT_snap_to_cursor);
  WM_operatortype_append(GREASE_PENCIL_OT_snap_cursor_to_selected);
  WM_operatortype_append(GREASE_PENCIL_OT_set_curve_type);
}
