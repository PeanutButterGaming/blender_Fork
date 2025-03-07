/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curves_to_grease_pencil_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").description("Either plain curves or curve instances");
  b.add_input<decl::Bool>("Selection")
      .default_value(true)
      .hide_value()
      .field_on_all()
      .description("Either a curve or instance selection");
  b.add_input<decl::Bool>("Instances as Layers")
      .default_value(true)
      .description("Create a separate layer for each instance");
  b.add_output<decl::Geometry>("Grease Pencil").propagate_all();
}

static GreasePencil *curves_to_grease_pencil_with_one_layer(
    const Curves &curves_id,
    const Field<bool> &selection_field,
    const StringRefNull layer_name,
    const AnonymousAttributePropagationInfo &propagation_info)
{
  bke::CurvesGeometry curves = curves_id.geometry.wrap();

  const bke::CurvesFieldContext field_context{curves, AttrDomain::Curve};
  FieldEvaluator evaluator{field_context, curves.curves_num()};
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const IndexMask curves_selection = evaluator.get_evaluated_selection_as_mask();
  IndexMaskMemory memory;
  const IndexMask curves_to_delete = curves_selection.complement(curves.curves_range(), memory);
  curves.remove_curves(curves_to_delete, propagation_info);

  GreasePencil *grease_pencil = BKE_grease_pencil_new_nomain();
  bke::greasepencil::Layer &layer = grease_pencil->add_layer(layer_name);
  bke::greasepencil::Drawing *drawing = grease_pencil->insert_frame(
      layer, grease_pencil->runtime->eval_frame);
  BLI_assert(drawing);
  drawing->strokes_for_write() = std::move(curves);

  /* Transfer materials. */
  const int materials_num = curves_id.totcol;
  grease_pencil->material_array_num = materials_num;
  grease_pencil->material_array = MEM_cnew_array<Material *>(materials_num, __func__);
  initialized_copy_n(curves_id.mat, materials_num, grease_pencil->material_array);

  return grease_pencil;
}

static GreasePencil *curve_instances_to_grease_pencil_layers(
    const bke::Instances &instances,
    const Field<bool> &selection_field,
    const AnonymousAttributePropagationInfo &propagation_info)
{
  const Span<int> reference_handles = instances.reference_handles();
  const Span<bke::InstanceReference> references = instances.references();
  const Span<float4x4> transforms = instances.transforms();

  const int instances_num = instances.instances_num();
  if (instances_num == 0) {
    return nullptr;
  }

  const bke::InstancesFieldContext field_context{instances};
  FieldEvaluator evaluator{field_context, instances_num};
  evaluator.set_selection(selection_field);
  evaluator.evaluate();
  const IndexMask instance_selection = evaluator.get_evaluated_selection_as_mask();

  const int layer_num = instance_selection.size();
  if (layer_num == 0) {
    return nullptr;
  }

  GreasePencil *grease_pencil = BKE_grease_pencil_new_nomain();

  VectorSet<Material *> all_materials;

  instance_selection.foreach_index([&](const int instance_i) {
    const bke::InstanceReference &reference = references[reference_handles[instance_i]];

    bke::greasepencil::Layer &layer = grease_pencil->add_layer(reference.name());
    grease_pencil->insert_frame(layer, grease_pencil->runtime->eval_frame);
    layer.set_local_transform(transforms[instance_i]);

    bke::greasepencil::Drawing *drawing = grease_pencil->get_eval_drawing(layer);
    BLI_assert(drawing);

    GeometrySet instance_geometry;
    reference.to_geometry_set(instance_geometry);
    const Curves *instance_curves = instance_geometry.get_curves();
    if (!instance_curves) {
      return;
    }

    bke::CurvesGeometry &strokes = drawing->strokes_for_write();
    strokes = instance_curves->geometry.wrap();

    Vector<int> new_material_indices;
    for (Material *material : Span{instance_curves->mat, instance_curves->totcol}) {
      new_material_indices.append(all_materials.index_of_or_add(material));
    }

    /* Remap material indices. */
    bke::SpanAttributeWriter<int> material_indices =
        strokes.attributes_for_write().lookup_or_add_for_write_span<int>("material_index",
                                                                         bke::AttrDomain::Curve);
    for (int &material_index : material_indices.span) {
      if (material_index >= 0 && material_index < new_material_indices.size()) {
        material_index = new_material_indices[material_index];
      }
    }
    material_indices.finish();
  });

  grease_pencil->material_array_num = all_materials.size();
  grease_pencil->material_array = MEM_cnew_array<Material *>(all_materials.size(), __func__);
  initialized_copy_n(all_materials.data(), all_materials.size(), grease_pencil->material_array);

  const bke::AttributeAccessor instances_attributes = instances.attributes();
  bke::MutableAttributeAccessor grease_pencil_attributes = grease_pencil->attributes_for_write();
  instances_attributes.for_all([&](const StringRef attribute_id,
                                   const AttributeMetaData &meta_data) {
    if (instances_attributes.is_builtin(attribute_id) &&
        !grease_pencil_attributes.is_builtin(attribute_id))
    {
      return true;
    }
    if (ELEM(attribute_id, "opacity")) {
      return true;
    }
    if (bke::attribute_name_is_anonymous(attribute_id) &&
        !propagation_info.propagate(attribute_id))
    {
      return true;
    }
    const GAttributeReader src_attribute = instances_attributes.lookup(attribute_id);
    if (!src_attribute) {
      return true;
    }
    if (instance_selection.size() == instances_num && src_attribute.varray.is_span() &&
        src_attribute.sharing_info)
    {
      /* Try reusing existing attribute array. */
      grease_pencil_attributes.add(
          attribute_id,
          AttrDomain::Layer,
          meta_data.data_type,
          bke::AttributeInitShared{src_attribute.varray.get_internal_span().data(),
                                   *src_attribute.sharing_info});
      return true;
    }
    if (!grease_pencil_attributes.add(
            attribute_id, AttrDomain::Layer, meta_data.data_type, bke::AttributeInitConstruct()))
    {
      return true;
    }
    bke::GSpanAttributeWriter dst_attribute = grease_pencil_attributes.lookup_for_write_span(
        attribute_id);
    array_utils::gather(src_attribute.varray, instance_selection, dst_attribute.span);
    dst_attribute.finish();
    return true;
  });

  {
    /* Manually propagate "opacity" data, because it's not a layer attribute on grease pencil
     * yet. Default to a full opacity of 1. */
    const VArray<float> opacities = *instances_attributes.lookup_or_default<float>(
        "opacity", AttrDomain::Instance, 1.0f);
    instance_selection.foreach_index([&](const int instance_i, const int layer_i) {
      grease_pencil->layer(layer_i)->opacity = opacities[instance_i];
    });
  }

  return grease_pencil;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet curves_geometry = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const bool instances_as_layers = params.extract_input<bool>("Instances as Layers");
  const AnonymousAttributePropagationInfo &propagation_info = params.get_output_propagation_info(
      "Grease Pencil");

  GreasePencil *grease_pencil = nullptr;
  if (instances_as_layers) {
    if (curves_geometry.has_curves()) {
      params.error_message_add(NodeWarningType::Info, TIP_("Non-instance curves are ignored"));
    }
    const bke::Instances *instances = curves_geometry.get_instances();
    if (!instances) {
      params.set_default_remaining_outputs();
      return;
    }
    grease_pencil = curve_instances_to_grease_pencil_layers(
        *instances, selection_field, propagation_info);
  }
  else {
    if (curves_geometry.has_instances()) {
      params.error_message_add(NodeWarningType::Info, TIP_("Instances are ignored"));
    }
    const Curves *curves_id = curves_geometry.get_curves();
    if (!curves_id) {
      params.set_default_remaining_outputs();
      return;
    }
    grease_pencil = curves_to_grease_pencil_with_one_layer(
        *curves_id, selection_field, curves_geometry.name, propagation_info);
  }

  GeometrySet grease_pencil_geometry = GeometrySet::from_grease_pencil(grease_pencil);
  grease_pencil_geometry.name = std::move(curves_geometry.name);
  params.set_output("Grease Pencil", std::move(grease_pencil_geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, GEO_NODE_CURVES_TO_GREASE_PENCIL, "Curves to Grease Pencil", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_type_size(&ntype, 160, 100, 320);

  bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curves_to_grease_pencil_cc
