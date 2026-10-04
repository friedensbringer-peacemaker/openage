// Copyright 2021-2025 the openage authors. See copying.md for legal info.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <nyan/nyan.h>

#include "curve/container/map.h"
#include "gamestate/component/api_component.h"
#include "gamestate/component/types.h"
#include "time/time.h"


namespace openage::gamestate::component {
class Live final : public APIComponent {
public:
	using APIComponent::APIComponent;

	component_t get_type() const override;

	/**
	 * Add a new attribute to the component attributes.
	 *
	 * @param time The time at which the attribute is added.
	 * @param attribute Attribute identifier (fqon of the nyan object).
	 * @param starting_values Attribute values at the time of addition.
	 */
	void add_attribute(const time::time_t &time,
	                   const nyan::fqon_t &attribute,
	                   std::shared_ptr<curve::Discrete<int64_t>> starting_values);

	/**
	 * Set the value of an attribute at a given time.
	 *
	 * @param time The time at which the attribute is set.
	 * @param attribute Attribute identifier (fqon of the nyan object).
	 * @param value New attribute value.
	 */
	void set_attribute(const time::time_t &time,
	                   const nyan::fqon_t &attribute,
	                   int64_t value);

	/**
	 * Get the value of an attribute at a given time (XR fork).
	 *
	 * @param time The time at which the attribute is read.
	 * @param attribute Attribute identifier (fqon of the nyan object).
	 *
	 * @return Attribute value, or nothing if the entity has no such attribute.
	 */
	std::optional<int64_t> get_attribute(const time::time_t &time,
	                                     const nyan::fqon_t &attribute) const;

private:
	using attribute_storage_t = curve::UnorderedMap<nyan::fqon_t,
	                                                std::shared_ptr<curve::Discrete<int64_t>>>;

	/**
	 * Map of attribute values by attribute type.
	 */
	attribute_storage_t attribute_values;
};

} // namespace openage::gamestate::component
