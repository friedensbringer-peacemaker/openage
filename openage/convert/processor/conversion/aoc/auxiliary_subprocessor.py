# Copyright 2020-2026 the openage authors. See copying.md for legal info.
#
# pylint: disable=line-too-long,too-many-locals,too-many-branches,too-many-statements,no-else-return

"""
Derives complex auxiliary objects from unit lines, techs
or other objects.
"""

from __future__ import annotations

import typing

from .....nyan.nyan_structs import MemberSpecialValue
from ....entity_object.conversion.aoc.genie_unit import (
    GenieBuildingLineGroup,
    GenieUnitLineGroup,
    GenieVillagerGroup,
)
from ....entity_object.conversion.combined_sound import CombinedSound
from ....entity_object.conversion.converter_object import ConverterObjectGroup, RawAPIObject
from ....service.conversion import internal_name_lookups
from ....service.read.nyan_api_loader import XR_GENIE_TASK_FIELDS, XR_GENIE_TECH_FIELDS, XR_GENIE_UNIT_FIELDS
from ....value_object.conversion.forward_ref import ForwardRef

if typing.TYPE_CHECKING:
    from openage.convert.entity_object.conversion.aoc.genie_tech import GenieTechEffectBundleGroup


class AoCAuxiliarySubprocessor:
    """
    Creates complexer auxiliary raw API objects for abilities in AoC.
    """

    @staticmethod
    def get_creatable_game_entity(line: GenieUnitLineGroup | GenieBuildingLineGroup) -> None:
        """
        Creates the CreatableGameEntity object for a unit/building line.

        :param line: Unit/Building line.
        :type line: ...dataformat.converter_object.ConverterObjectGroup
        """
        if isinstance(line, GenieVillagerGroup):
            current_unit = line.variants[0].line[0]

        else:
            current_unit = line.line[0]

        current_unit_id = line.get_head_unit_id()
        dataset = line.data

        name_lookup_dict = internal_name_lookups.get_entity_lookups(dataset.game_version)
        civ_lookup_dict = internal_name_lookups.get_civ_lookups(dataset.game_version)

        game_entity_name = name_lookup_dict[current_unit_id][0]

        obj_ref = f"{game_entity_name}.CreatableGameEntity"
        obj_name = f"{game_entity_name}Creatable"
        creatable_raw_api_object = RawAPIObject(obj_ref, obj_name, dataset.nyan_api_objects)
        creatable_raw_api_object.add_raw_parent("engine.util.create.CreatableGameEntity")

        # Get train location of line
        train_location_id = line.get_train_location_id()
        assert train_location_id is not None
        if isinstance(line, GenieBuildingLineGroup):
            train_location = dataset.unit_lines[train_location_id]

        else:
            train_location = dataset.building_lines[train_location_id]

        # Derive the game-entity name from the train_location's head unit,
        # not from train_location_id: the latter may not be a head unit (it
        # can be a non-head member of a line), in which case the id-based
        # lookup would not match the resolved group's actual game entity.
        train_location_name = name_lookup_dict[train_location.get_head_unit_id()][0]

        # Location of the object depends on whether it'a a unique unit or a normal unit
        if line.is_unique():
            # Add object to the Civ object
            enabling_research_id = line.get_enabling_research_id()
            enabling_research = dataset.genie_techs[enabling_research_id]
            enabling_civ_id = enabling_research["civilization_id"].value

            civ = dataset.civ_groups[enabling_civ_id]
            civ_name = civ_lookup_dict[enabling_civ_id][0]

            creatable_location = ForwardRef(civ, civ_name)

        else:
            # Add object to the train location's Create ability
            creatable_location = ForwardRef(train_location, f"{train_location_name}.Create")

        creatable_raw_api_object.set_location(creatable_location)

        # Game Entity
        game_entity_forward_ref = ForwardRef(line, game_entity_name)
        creatable_raw_api_object.add_raw_member(
            "game_entity", game_entity_forward_ref, "engine.util.create.CreatableGameEntity"
        )

        # TODO: Variants
        variants_set = []

        creatable_raw_api_object.add_raw_member(
            "variants", variants_set, "engine.util.create.CreatableGameEntity"
        )

        # Cost (construction)
        cost_name = f"{game_entity_name}.CreatableGameEntity.{game_entity_name}Cost"
        cost_raw_api_object = RawAPIObject(cost_name, f"{game_entity_name}Cost", dataset.nyan_api_objects)
        cost_raw_api_object.add_raw_parent("engine.util.cost.type.ResourceCost")
        creatable_forward_ref = ForwardRef(line, obj_ref)
        cost_raw_api_object.set_location(creatable_forward_ref)

        payment_mode = dataset.nyan_api_objects["engine.util.payment_mode.type.Advance"]
        cost_raw_api_object.add_raw_member("payment_mode", payment_mode, "engine.util.cost.Cost")

        if line.is_repairable():
            # Cost (repair) for buildings
            cost_repair_name = f"{game_entity_name}.CreatableGameEntity.{game_entity_name}RepairCost"
            cost_repair_raw_api_object = RawAPIObject(
                cost_repair_name, f"{game_entity_name}RepairCost", dataset.nyan_api_objects
            )
            cost_repair_raw_api_object.add_raw_parent("engine.util.cost.type.ResourceCost")
            creatable_forward_ref = ForwardRef(line, obj_ref)
            cost_repair_raw_api_object.set_location(creatable_forward_ref)

            payment_repair_mode = dataset.nyan_api_objects["engine.util.payment_mode.type.Adaptive"]
            cost_repair_raw_api_object.add_raw_member(
                "payment_mode", payment_repair_mode, "engine.util.cost.Cost"
            )
            line.add_raw_api_object(cost_repair_raw_api_object)

        cost_amounts = []
        cost_repair_amounts = []
        for resource_amount in current_unit["resource_cost"].value:
            resource_id = resource_amount["type_id"].value

            resource = None
            resource_name = ""
            if resource_id == -1:
                # Not a valid resource
                continue

            if resource_id == 0:
                resource = dataset.pregen_nyan_objects["util.resource.types.Food"].get_nyan_object()
                resource_name = "Food"

            elif resource_id == 1:
                resource = dataset.pregen_nyan_objects["util.resource.types.Wood"].get_nyan_object()
                resource_name = "Wood"

            elif resource_id == 2:
                resource = dataset.pregen_nyan_objects["util.resource.types.Stone"].get_nyan_object()
                resource_name = "Stone"

            elif resource_id == 3:
                resource = dataset.pregen_nyan_objects["util.resource.types.Gold"].get_nyan_object()
                resource_name = "Gold"

            else:
                # Other resource ids are handled differently
                continue

            # Skip resources that are only expected to be there
            if not resource_amount["enabled"].value:
                continue

            amount = resource_amount["amount"].value

            cost_amount_name = f"{cost_name}.{resource_name}Amount"
            cost_amount = RawAPIObject(cost_amount_name, f"{resource_name}Amount", dataset.nyan_api_objects)
            cost_amount.add_raw_parent("engine.util.resource.ResourceAmount")
            cost_forward_ref = ForwardRef(line, cost_name)
            cost_amount.set_location(cost_forward_ref)

            cost_amount.add_raw_member("type", resource, "engine.util.resource.ResourceAmount")
            cost_amount.add_raw_member("amount", amount, "engine.util.resource.ResourceAmount")

            cost_amount_forward_ref = ForwardRef(line, cost_amount_name)
            cost_amounts.append(cost_amount_forward_ref)
            line.add_raw_api_object(cost_amount)

            if line.is_repairable():
                # Cost for repairing = half of the construction cost
                cost_amount_name = f"{cost_repair_name}.{resource_name}Amount"
                cost_amount = RawAPIObject(
                    cost_amount_name, f"{resource_name}Amount", dataset.nyan_api_objects
                )
                cost_amount.add_raw_parent("engine.util.resource.ResourceAmount")
                cost_forward_ref = ForwardRef(line, cost_repair_name)
                cost_amount.set_location(cost_forward_ref)

                cost_amount.add_raw_member("type", resource, "engine.util.resource.ResourceAmount")
                cost_amount.add_raw_member("amount", amount / 2, "engine.util.resource.ResourceAmount")

                cost_amount_forward_ref = ForwardRef(line, cost_amount_name)
                cost_repair_amounts.append(cost_amount_forward_ref)
                line.add_raw_api_object(cost_amount)

        cost_raw_api_object.add_raw_member("amount", cost_amounts, "engine.util.cost.type.ResourceCost")

        if line.is_repairable():
            cost_repair_raw_api_object.add_raw_member(
                "amount", cost_repair_amounts, "engine.util.cost.type.ResourceCost"
            )

        cost_forward_ref = ForwardRef(line, cost_name)
        creatable_raw_api_object.add_raw_member(
            "cost", cost_forward_ref, "engine.util.create.CreatableGameEntity"
        )
        # Creation time
        if isinstance(line, GenieUnitLineGroup):
            creation_time = current_unit["creation_time"].value

        else:
            # Buildings are created immediately
            creation_time = 0

        creatable_raw_api_object.add_raw_member(
            "creation_time", creation_time, "engine.util.create.CreatableGameEntity"
        )

        # Creation sound
        creation_sound_id = current_unit["train_sound_id"].value

        # Create sound object
        obj_name = f"{game_entity_name}.CreatableGameEntity.Sound"
        sound_raw_api_object = RawAPIObject(obj_name, "CreationSound", dataset.nyan_api_objects)
        sound_raw_api_object.add_raw_parent("engine.util.sound.Sound")
        sound_location = ForwardRef(line, obj_ref)
        sound_raw_api_object.set_location(sound_location)

        # Search for the sound if it exists
        creation_sounds = []
        if creation_sound_id > -1:
            # Creation sound should be civ agnostic
            genie_sound = dataset.genie_sounds[creation_sound_id]
            file_id = genie_sound.get_sounds(civ_id=-1)[0]

            if file_id in dataset.combined_sounds:
                creation_sound = dataset.combined_sounds[file_id]
                creation_sound.add_reference(sound_raw_api_object)

            else:
                creation_sound = CombinedSound(
                    creation_sound_id, file_id, f"creation_sound_{creation_sound_id}", dataset
                )
                dataset.combined_sounds.update({file_id: creation_sound})
                creation_sound.add_reference(sound_raw_api_object)

            creation_sounds.append(creation_sound)

        sound_raw_api_object.add_raw_member("play_delay", 0, "engine.util.sound.Sound")
        sound_raw_api_object.add_raw_member("sounds", creation_sounds, "engine.util.sound.Sound")

        sound_forward_ref = ForwardRef(line, obj_name)
        creatable_raw_api_object.add_raw_member(
            "creation_sounds", [sound_forward_ref], "engine.util.create.CreatableGameEntity"
        )

        line.add_raw_api_object(sound_raw_api_object)

        # Condition
        unlock_conditions = []
        enabling_research_id = line.get_enabling_research_id()
        if enabling_research_id > -1:
            unlock_conditions.extend(
                AoCAuxiliarySubprocessor.get_condition(line, obj_ref, enabling_research_id)
            )

        creatable_raw_api_object.add_raw_member(
            "condition", unlock_conditions, "engine.util.create.CreatableGameEntity"
        )

        # Placement modes
        placement_modes = []
        if isinstance(line, GenieBuildingLineGroup):
            # Buildings are placed on the map
            # Place mode
            obj_name = f"{game_entity_name}.CreatableGameEntity.Place"
            place_raw_api_object = RawAPIObject(obj_name, "Place", dataset.nyan_api_objects)
            place_raw_api_object.add_raw_parent("engine.util.placement_mode.type.Place")
            place_location = ForwardRef(line, f"{game_entity_name}.CreatableGameEntity")
            place_raw_api_object.set_location(place_location)

            # Tile snap distance (uses 1.0 for grid placement)
            place_raw_api_object.add_raw_member(
                "tile_snap_distance", 1.0, "engine.util.placement_mode.type.Place"
            )
            # Clearance size
            clearance_size_x = current_unit["clearance_size_x"].value
            clearance_size_y = current_unit["clearance_size_y"].value
            place_raw_api_object.add_raw_member(
                "clearance_size_x", clearance_size_x, "engine.util.placement_mode.type.Place"
            )
            place_raw_api_object.add_raw_member(
                "clearance_size_y", clearance_size_y, "engine.util.placement_mode.type.Place"
            )

            # Allow rotation
            place_raw_api_object.add_raw_member(
                "allow_rotation", True, "engine.util.placement_mode.type.Place"
            )

            # Max elevation difference
            elevation_mode = current_unit["elevation_mode"].value
            if elevation_mode == 2:
                max_elevation_difference = 0

            elif elevation_mode == 3:
                max_elevation_difference = 1

            else:
                max_elevation_difference = MemberSpecialValue.NYAN_INF

            place_raw_api_object.add_raw_member(
                "max_elevation_difference", max_elevation_difference, "engine.util.placement_mode.type.Place"
            )

            line.add_raw_api_object(place_raw_api_object)

            place_forward_ref = ForwardRef(line, obj_name)
            placement_modes.append(place_forward_ref)

            if line.get_class_id() == 39:
                # Gates
                obj_name = f"{game_entity_name}.CreatableGameEntity.Replace"
                replace_raw_api_object = RawAPIObject(obj_name, "Replace", dataset.nyan_api_objects)
                replace_raw_api_object.add_raw_parent("engine.util.placement_mode.type.Replace")
                replace_location = ForwardRef(line, f"{game_entity_name}.CreatableGameEntity")
                replace_raw_api_object.set_location(replace_location)

                # Game entities (only stone wall)
                wall_line_id = 117
                wall_line = dataset.building_lines[wall_line_id]
                wall_name = name_lookup_dict[117][0]
                game_entities = [ForwardRef(wall_line, wall_name)]
                replace_raw_api_object.add_raw_member(
                    "game_entities", game_entities, "engine.util.placement_mode.type.Replace"
                )

                line.add_raw_api_object(replace_raw_api_object)

                replace_forward_ref = ForwardRef(line, obj_name)
                placement_modes.append(replace_forward_ref)

        else:
            placement_modes.append(dataset.nyan_api_objects["engine.util.placement_mode.type.Eject"])

            # OwnStorage mode
            obj_name = f"{game_entity_name}.CreatableGameEntity.OwnStorage"
            own_storage_raw_api_object = RawAPIObject(obj_name, "OwnStorage", dataset.nyan_api_objects)
            own_storage_raw_api_object.add_raw_parent("engine.util.placement_mode.type.OwnStorage")
            own_storage_location = ForwardRef(line, f"{game_entity_name}.CreatableGameEntity")
            own_storage_raw_api_object.set_location(own_storage_location)

            # Container
            container_forward_ref = ForwardRef(
                train_location, (f"{train_location_name}.Storage.{train_location_name}Container")
            )
            own_storage_raw_api_object.add_raw_member(
                "container", container_forward_ref, "engine.util.placement_mode.type.OwnStorage"
            )

            line.add_raw_api_object(own_storage_raw_api_object)

            own_storage_forward_ref = ForwardRef(line, obj_name)
            placement_modes.append(own_storage_forward_ref)

        creatable_raw_api_object.add_raw_member(
            "placement_modes", placement_modes, "engine.util.create.CreatableGameEntity"
        )

        line.add_raw_api_object(creatable_raw_api_object)
        line.add_raw_api_object(cost_raw_api_object)

    @staticmethod
    def get_researchable_tech(tech_group: GenieTechEffectBundleGroup) -> None:
        """
        Creates the ResearchableTech object for a Tech.

        :param tech_group: Tech group that is a technology.
        :type tech_group: ...dataformat.converter_object.ConverterObjectGroup
        """
        dataset = tech_group.data
        research_location_id = tech_group.get_research_location_id()
        assert research_location_id is not None
        research_location = dataset.building_lines[research_location_id]

        name_lookup_dict = internal_name_lookups.get_entity_lookups(dataset.game_version)
        tech_lookup_dict = internal_name_lookups.get_tech_lookups(dataset.game_version)
        civ_lookup_dict = internal_name_lookups.get_civ_lookups(dataset.game_version)

        research_location_name = name_lookup_dict[research_location.get_head_unit_id()][0]
        tech_name = tech_lookup_dict[tech_group.get_id()][0]

        obj_ref = f"{tech_name}.ResearchableTech"
        obj_name = f"{tech_name}Researchable"
        researchable_raw_api_object = RawAPIObject(obj_ref, obj_name, dataset.nyan_api_objects)
        researchable_raw_api_object.add_raw_parent("engine.util.research.ResearchableTech")

        # Location of the object depends on whether it'a a unique tech or a normal tech
        if tech_group.is_unique():
            # Add object to the Civ object
            civ_id = tech_group.get_civilization()
            assert civ_id is not None
            civ = dataset.civ_groups[civ_id]
            civ_name = civ_lookup_dict[civ_id][0]

            researchable_location = ForwardRef(civ, civ_name)

        else:
            # Add object to the research location's Research ability
            researchable_location = ForwardRef(research_location, f"{research_location_name}.Research")

        researchable_raw_api_object.set_location(researchable_location)

        # Tech
        tech_forward_ref = ForwardRef(tech_group, tech_name)
        researchable_raw_api_object.add_raw_member(
            "tech", tech_forward_ref, "engine.util.research.ResearchableTech"
        )

        # Cost
        cost_ref = f"{tech_name}.ResearchableTech.{tech_name}Cost"
        cost_raw_api_object = RawAPIObject(cost_ref, f"{tech_name}Cost", dataset.nyan_api_objects)
        cost_raw_api_object.add_raw_parent("engine.util.cost.type.ResourceCost")
        tech_forward_ref = ForwardRef(tech_group, obj_ref)
        cost_raw_api_object.set_location(tech_forward_ref)

        payment_mode = dataset.nyan_api_objects["engine.util.payment_mode.type.Advance"]
        cost_raw_api_object.add_raw_member("payment_mode", payment_mode, "engine.util.cost.Cost")

        cost_amounts = []
        for resource_amount in tech_group.tech["research_resource_costs"].value:
            resource_id = resource_amount["type_id"].value
            resource = None
            resource_name = ""
            if resource_id == -1:
                # Not a valid resource
                continue

            if resource_id == 0:
                resource = dataset.pregen_nyan_objects["util.resource.types.Food"].get_nyan_object()
                resource_name = "Food"

            elif resource_id == 1:
                resource = dataset.pregen_nyan_objects["util.resource.types.Wood"].get_nyan_object()
                resource_name = "Wood"

            elif resource_id == 2:
                resource = dataset.pregen_nyan_objects["util.resource.types.Stone"].get_nyan_object()
                resource_name = "Stone"

            elif resource_id == 3:
                resource = dataset.pregen_nyan_objects["util.resource.types.Gold"].get_nyan_object()
                resource_name = "Gold"

            else:
                # Other resource ids are handled differently
                continue

            # Skip resources that are only expected to be there
            if not resource_amount["enabled"].value:
                continue

            amount = resource_amount["amount"].value

            cost_amount_ref = f"{cost_ref}.{resource_name}Amount"
            cost_amount = RawAPIObject(cost_amount_ref, f"{resource_name}Amount", dataset.nyan_api_objects)
            cost_amount.add_raw_parent("engine.util.resource.ResourceAmount")
            cost_forward_ref = ForwardRef(tech_group, cost_ref)
            cost_amount.set_location(cost_forward_ref)

            cost_amount.add_raw_member("type", resource, "engine.util.resource.ResourceAmount")
            cost_amount.add_raw_member("amount", amount, "engine.util.resource.ResourceAmount")

            cost_amount_forward_ref = ForwardRef(tech_group, cost_amount_ref)
            cost_amounts.append(cost_amount_forward_ref)
            tech_group.add_raw_api_object(cost_amount)

        cost_raw_api_object.add_raw_member("amount", cost_amounts, "engine.util.cost.type.ResourceCost")

        cost_forward_ref = ForwardRef(tech_group, cost_ref)
        researchable_raw_api_object.add_raw_member(
            "cost", cost_forward_ref, "engine.util.research.ResearchableTech"
        )

        research_time = tech_group.tech["research_time"].value
        researchable_raw_api_object.add_raw_member(
            "research_time", research_time, "engine.util.research.ResearchableTech"
        )

        # Create sound object
        sound_ref = f"{tech_name}.ResearchableTech.Sound"
        sound_raw_api_object = RawAPIObject(sound_ref, "ResearchSound", dataset.nyan_api_objects)
        sound_raw_api_object.add_raw_parent("engine.util.sound.Sound")
        sound_location = ForwardRef(tech_group, f"{tech_name}.ResearchableTech")
        sound_raw_api_object.set_location(sound_location)

        # AoE doesn't support sounds here, so this is empty
        sound_raw_api_object.add_raw_member("play_delay", 0, "engine.util.sound.Sound")
        sound_raw_api_object.add_raw_member("sounds", [], "engine.util.sound.Sound")

        sound_forward_ref = ForwardRef(tech_group, sound_ref)
        researchable_raw_api_object.add_raw_member(
            "research_sounds", [sound_forward_ref], "engine.util.research.ResearchableTech"
        )

        tech_group.add_raw_api_object(sound_raw_api_object)

        # Condition
        unlock_conditions = []
        if tech_group.get_id() > -1:
            unlock_conditions.extend(
                AoCAuxiliarySubprocessor.get_condition(
                    tech_group, obj_ref, tech_group.get_id(), top_level=True
                )
            )

        researchable_raw_api_object.add_raw_member(
            "condition", unlock_conditions, "engine.util.research.ResearchableTech"
        )

        tech_group.add_raw_api_object(researchable_raw_api_object)
        tech_group.add_raw_api_object(cost_raw_api_object)

    @staticmethod
    def get_condition(
        converter_obj_group: ConverterObjectGroup, obj_ref: str, tech_id: int, top_level: bool = False
    ) -> list[ForwardRef]:
        """
        Creates the condition for a creatable or researchable from tech
        by recursively searching the required techs.

        :param converter_object: ConverterObjectGroup that the condition objects should be nested in.
        :param obj_ref: Reference of converter_object inside the modpack.
        :param tech_id: tech ID of a tech wth a conditional unlock.
        :param top_level: True if the condition has subconditions, False otherwise.
        """
        dataset = converter_obj_group.data
        tech = dataset.genie_techs[tech_id]

        name_lookup_dict = internal_name_lookups.get_entity_lookups(dataset.game_version)
        tech_lookup_dict = internal_name_lookups.get_tech_lookups(dataset.game_version)

        if not top_level and (
            tech_id in dataset.initiated_techs.keys()
            or (tech_id in dataset.tech_groups.keys() and dataset.tech_groups[tech_id].is_researchable())
        ):
            # The tech condition is a building or a researchable tech
            # and thus a literal.
            if tech_id in dataset.initiated_techs.keys():
                initiated_tech = dataset.initiated_techs[tech_id]
                building_id = initiated_tech.get_building_id()
                building_name = name_lookup_dict[building_id][0]
                literal_name = f"{building_name}Built"
                literal_parent = "engine.util.logic.literal.type.GameEntityProgress"

            elif dataset.tech_groups[tech_id].is_researchable():
                tech_name = tech_lookup_dict[tech_id][0]
                literal_name = f"{tech_name}Researched"
                literal_parent = "engine.util.logic.literal.type.TechResearched"

            else:
                raise ValueError("Required tech id {tech_id} is neither intiated nor researchable")

            literal_ref = f"{obj_ref}.{literal_name}"
            literal_raw_api_object = RawAPIObject(literal_ref, literal_name, dataset.nyan_api_objects)
            literal_raw_api_object.add_raw_parent(literal_parent)
            literal_location = ForwardRef(converter_obj_group, obj_ref)
            literal_raw_api_object.set_location(literal_location)

            if tech_id in dataset.initiated_techs.keys():
                building_line = dataset.unit_ref[building_id]
                building_forward_ref = ForwardRef(building_line, building_name)

                # Building
                literal_raw_api_object.add_raw_member("game_entity", building_forward_ref, literal_parent)

                # Progress
                # =======================================================================
                progress_ref = f"{literal_ref}.ProgressStatus"
                progress_raw_api_object = RawAPIObject(
                    progress_ref, "ProgressStatus", dataset.nyan_api_objects
                )
                progress_raw_api_object.add_raw_parent("engine.util.progress_status.ProgressStatus")
                progress_location = ForwardRef(converter_obj_group, literal_ref)
                progress_raw_api_object.set_location(progress_location)

                # Type
                progress_type = dataset.nyan_api_objects["engine.util.progress_type.type.Construct"]
                progress_raw_api_object.add_raw_member(
                    "progress_type", progress_type, "engine.util.progress_status.ProgressStatus"
                )

                # Progress (building must be 100% constructed)
                progress_raw_api_object.add_raw_member(
                    "progress", 100, "engine.util.progress_status.ProgressStatus"
                )

                converter_obj_group.add_raw_api_object(progress_raw_api_object)
                # =======================================================================
                progress_forward_ref = ForwardRef(converter_obj_group, progress_ref)
                literal_raw_api_object.add_raw_member("progress_status", progress_forward_ref, literal_parent)

            elif dataset.tech_groups[tech_id].is_researchable():
                tech_group = dataset.tech_groups[tech_id]
                tech_forward_ref = ForwardRef(tech_group, tech_name)
                literal_raw_api_object.add_raw_member("tech", tech_forward_ref, literal_parent)

            # LiteralScope
            # ==========================================================================
            scope_ref = f"{literal_ref}.LiteralScope"
            scope_raw_api_object = RawAPIObject(scope_ref, "LiteralScope", dataset.nyan_api_objects)
            scope_raw_api_object.add_raw_parent("engine.util.logic.literal_scope.type.Any")
            scope_location = ForwardRef(converter_obj_group, literal_ref)
            scope_raw_api_object.set_location(scope_location)

            scope_diplomatic_stances = [dataset.nyan_api_objects["engine.util.diplomatic_stance.type.Self"]]
            scope_raw_api_object.add_raw_member(
                "stances", scope_diplomatic_stances, "engine.util.logic.literal_scope.LiteralScope"
            )

            converter_obj_group.add_raw_api_object(scope_raw_api_object)
            # ==========================================================================
            scope_forward_ref = ForwardRef(converter_obj_group, scope_ref)
            literal_raw_api_object.add_raw_member(
                "scope", scope_forward_ref, "engine.util.logic.literal.Literal"
            )

            literal_raw_api_object.add_raw_member("only_once", True, "engine.util.logic.LogicElement")

            converter_obj_group.add_raw_api_object(literal_raw_api_object)
            literal_forward_ref = ForwardRef(converter_obj_group, literal_ref)

            return [literal_forward_ref]

        else:
            # The tech condition has other requirements that need to be resolved

            # Find required techs for the current tech
            assoc_tech_id_members = []
            assoc_tech_id_members.extend(tech["required_techs"].value)
            required_tech_count = tech["required_tech_count"].value

            # Remove tech ids that are invalid or those we don't use
            relevant_ids = []
            for tech_id_member in assoc_tech_id_members:
                required_tech_id = tech_id_member.value
                if required_tech_id == -1:
                    continue

                if required_tech_id == 104:
                    # Skip Dark Age tech
                    required_tech_count -= 1
                    continue

                if required_tech_id == tech_id:
                    # a tech requiring itself would recurse forever
                    required_tech_count -= 1
                    continue

                if required_tech_id in dataset.civ_boni.keys():
                    continue

                relevant_ids.append(required_tech_id)

            if len(relevant_ids) == 0:
                return []

            if len(relevant_ids) == 1:
                # If there's only one required tech we don't need a gate
                # we can just return the logic element of the only required tech
                required_tech_id = relevant_ids[0]
                return AoCAuxiliarySubprocessor.get_condition(converter_obj_group, obj_ref, required_tech_id)

            gate_ref = f"{obj_ref}.UnlockCondition"
            gate_raw_api_object = RawAPIObject(gate_ref, "UnlockCondition", dataset.nyan_api_objects)

            if required_tech_count == len(relevant_ids):
                gate_raw_api_object.add_raw_parent("engine.util.logic.gate.type.AND")
                gate_location = ForwardRef(converter_obj_group, obj_ref)

            else:
                gate_raw_api_object.add_raw_parent("engine.util.logic.gate.type.SUBSETMIN")
                gate_location = ForwardRef(converter_obj_group, obj_ref)

                gate_raw_api_object.add_raw_member(
                    "size", required_tech_count, "engine.util.logic.gate.type.SUBSETMIN"
                )

            gate_raw_api_object.set_location(gate_location)

            # Once unlocked, a creatable/researchable is unlocked forever
            gate_raw_api_object.add_raw_member("only_once", True, "engine.util.logic.LogicElement")

            # Get requirements from subtech recursively
            inputs = []
            for required_tech_id in relevant_ids:
                required = AoCAuxiliarySubprocessor.get_condition(
                    converter_obj_group, gate_ref, required_tech_id
                )
                inputs.extend(required)

            gate_raw_api_object.add_raw_member("inputs", inputs, "engine.util.logic.gate.LogicGate")

            converter_obj_group.add_raw_api_object(gate_raw_api_object)
            gate_forward_ref = ForwardRef(converter_obj_group, gate_ref)
            return [gate_forward_ref]


class AoCXrSubprocessor:
    """
    XR fork (xr.ages): fills the engine.xr.* extension objects with raw Genie
    fields that the openage API does not model (see nyan_api_loader.py and
    XR-openage docs/KONVERTER-FELDER.md).
    """

    @staticmethod
    def _value(obj, member: str, default):
        """
        Raw value of a Genie member, default if the struct has no such member.
        """
        if hasattr(obj, "has_member"):
            present = obj.has_member(member)

        else:
            present = member in obj.value

        if not present:
            return default

        value = obj[member].value
        if isinstance(value, bool):
            return int(value)

        return value

    @classmethod
    def _add_fields(cls, raw_api_object: RawAPIObject, genie_obj, fields, origin: str) -> None:
        """
        Primitive members from a field table of nyan_api_loader.
        """
        for name, type_name, default, dat_name in fields:
            value = cls._value(genie_obj, dat_name, default)
            if type_name == "int":
                value = int(value)

            else:
                value = float(value)

            raw_api_object.add_raw_member(name, value, origin)

    @classmethod
    def _task_refs(
        cls,
        group: ConverterObjectGroup,
        location_ref: str,
        prefix: str,
        genie_unit,
        commands=None,
    ) -> list[ForwardRef]:
        """
        engine.xr.util.GenieTask objects nested in location_ref, one per unit command.
        """
        dataset = group.data
        if commands is None:
            commands = cls._value(genie_unit, "unit_commands", [])

        unit_id = cls._value(genie_unit, "id0", -1)
        refs = []
        for index, command in enumerate(commands):
            task_ref = f"{location_ref}.{prefix}{index}"
            task_raw_api_object = RawAPIObject(task_ref, f"{prefix}{index}", dataset.nyan_api_objects)
            task_raw_api_object.add_raw_parent("engine.xr.util.GenieTask")
            task_raw_api_object.set_location(ForwardRef(group, location_ref))

            task_raw_api_object.add_raw_member("source_unit_id", unit_id, "engine.xr.util.GenieTask")
            cls._add_fields(task_raw_api_object, command, XR_GENIE_TASK_FIELDS, "engine.xr.util.GenieTask")

            group.add_raw_api_object(task_raw_api_object)
            refs.append(ForwardRef(group, task_ref))

        return refs

    @classmethod
    def _fill_unit_data(cls, group: ConverterObjectGroup, raw_api_object: RawAPIObject, genie_unit) -> None:
        """
        All engine.xr.util.GenieUnitData members of one Genie unit.
        """
        dataset = group.data
        origin = "engine.xr.util.GenieUnitData"
        obj_ref = raw_api_object.get_id()

        cls._add_fields(raw_api_object, genie_unit, XR_GENIE_UNIT_FIELDS, origin)

        drop_sites = [member.value for member in cls._value(genie_unit, "drop_sites", [])]
        raw_api_object.add_raw_member("drop_sites", [site for site in drop_sites if site > -1], origin)

        storages = []
        for index, storage in enumerate(cls._value(genie_unit, "resource_storage", [])):
            storage_type = cls._value(storage, "type", -1)
            if storage_type < 0:
                continue

            storage_ref = f"{obj_ref}.Storage{index}"
            storage_raw_api_object = RawAPIObject(storage_ref, f"Storage{index}", dataset.nyan_api_objects)
            storage_raw_api_object.add_raw_parent("engine.xr.util.GenieResourceStorage")
            storage_raw_api_object.set_location(ForwardRef(group, obj_ref))
            storage_origin = "engine.xr.util.GenieResourceStorage"
            storage_raw_api_object.add_raw_member("type", storage_type, storage_origin)
            storage_raw_api_object.add_raw_member(
                "amount", float(cls._value(storage, "amount", 0.0)), storage_origin
            )
            storage_raw_api_object.add_raw_member(
                "used_mode", int(cls._value(storage, "used_mode", 0)), storage_origin
            )
            group.add_raw_api_object(storage_raw_api_object)
            storages.append(ForwardRef(group, storage_ref))

        raw_api_object.add_raw_member("resource_storages", storages, origin)

        annexes = []
        for index, annex in enumerate(cls._value(genie_unit, "building_annex", [])):
            annex_unit_id = cls._value(annex, "unit_id", -1)
            if annex_unit_id < 0:
                continue

            annex_ref = f"{obj_ref}.Annex{index}"
            annex_raw_api_object = RawAPIObject(annex_ref, f"Annex{index}", dataset.nyan_api_objects)
            annex_raw_api_object.add_raw_parent("engine.xr.util.GenieAnnex")
            annex_raw_api_object.set_location(ForwardRef(group, obj_ref))
            annex_origin = "engine.xr.util.GenieAnnex"
            annex_raw_api_object.add_raw_member("unit_id", annex_unit_id, annex_origin)
            annex_raw_api_object.add_raw_member(
                "offset_x", float(cls._value(annex, "misplaced0", 0.0)), annex_origin
            )
            annex_raw_api_object.add_raw_member(
                "offset_y", float(cls._value(annex, "misplaced1", 0.0)), annex_origin
            )
            group.add_raw_api_object(annex_raw_api_object)
            annexes.append(ForwardRef(group, annex_ref))

        raw_api_object.add_raw_member("annexes", annexes, origin)

        tasks = cls._task_refs(group, obj_ref, "Task", genie_unit)
        raw_api_object.add_raw_member("tasks", tasks, origin)

    @staticmethod
    def _variant_units(line: ConverterObjectGroup) -> list:
        """
        Genie units that belong to the game entity besides the head unit
        (villager task units, monk with relic, packed/unpacked, stacked building).
        """
        head_unit = line.get_head_unit()
        units = []
        if isinstance(line, GenieVillagerGroup):
            for variant in line.variants:
                units.extend(variant.line)

        for attr in ("transform_unit", "switch_unit", "stack"):
            unit = getattr(line, attr, None)
            if unit is not None:
                units.append(unit)

        result = []
        seen = {head_unit["id0"].value}
        for unit in units:
            unit_id = unit["id0"].value
            if unit_id not in seen:
                seen.add(unit_id)
                result.append(unit)

        return result

    @classmethod
    def genie_unit_ability(
        cls,
        line: ConverterObjectGroup,
        entity_ref: str | None = None,
        genie_unit=None,
    ) -> ForwardRef:
        """
        Adds the engine.xr.ability.GenieUnit ability to a game entity.

        :param line: Group that owns the game entity.
        :param entity_ref: Raw API object ID of the game entity (default: the line's entity).
        :param genie_unit: Genie unit of the entity (default: the line's head unit).
        """
        dataset = line.data
        if entity_ref is None:
            name_lookup_dict = internal_name_lookups.get_entity_lookups(dataset.game_version)
            entity_ref = name_lookup_dict[line.get_head_unit_id()][0]

        variants_units = []
        line_unit_ids = []
        if genie_unit is None:
            genie_unit = line.get_head_unit()
            variants_units = cls._variant_units(line)
            for unit in getattr(line, "line", []):
                unit_id = unit["id0"].value
                if unit_id not in line_unit_ids:
                    line_unit_ids.append(unit_id)

        ability_ref = f"{entity_ref}.GenieUnit"
        ability_raw_api_object = RawAPIObject(ability_ref, "GenieUnit", dataset.nyan_api_objects)
        ability_raw_api_object.add_raw_parent("engine.xr.ability.GenieUnit")
        ability_raw_api_object.set_location(ForwardRef(line, entity_ref))
        line.add_raw_api_object(ability_raw_api_object)

        cls._fill_unit_data(line, ability_raw_api_object, genie_unit)

        variants = []
        for unit in variants_units:
            variant_ref = f"{ability_ref}.Variant{unit['id0'].value}"
            variant_raw_api_object = RawAPIObject(
                variant_ref, f"Variant{unit['id0'].value}", dataset.nyan_api_objects
            )
            variant_raw_api_object.add_raw_parent("engine.xr.util.GenieUnitData")
            variant_raw_api_object.set_location(ForwardRef(line, ability_ref))
            line.add_raw_api_object(variant_raw_api_object)
            cls._fill_unit_data(line, variant_raw_api_object, unit)
            variants.append(ForwardRef(line, variant_ref))

        ability_raw_api_object.add_raw_member("variants", variants, "engine.xr.ability.GenieUnit")
        ability_raw_api_object.add_raw_member("line_unit_ids", line_unit_ids, "engine.xr.ability.GenieUnit")

        return ForwardRef(line, ability_ref)

    @classmethod
    def genie_tasks_property(
        cls,
        line: ConverterObjectGroup,
        ability_ref: str,
        genie_unit,
        commands: list,
    ) -> tuple[typing.Any, ForwardRef]:
        """
        engine.xr.property.GenieTasks for an ability that implements Genie tasks.

        :returns: (property key, forward reference) for Ability.properties
        """
        dataset = line.data
        property_ref = f"{ability_ref}.GenieTasks"
        property_raw_api_object = RawAPIObject(property_ref, "GenieTasks", dataset.nyan_api_objects)
        property_raw_api_object.add_raw_parent("engine.xr.property.GenieTasks")
        property_raw_api_object.set_location(ForwardRef(line, ability_ref))
        line.add_raw_api_object(property_raw_api_object)

        tasks = cls._task_refs(line, property_ref, "Task", genie_unit, commands)
        property_raw_api_object.add_raw_member("tasks", tasks, "engine.xr.property.GenieTasks")

        key = dataset.nyan_api_objects["engine.xr.property.GenieTasks"]
        return key, ForwardRef(line, property_ref)

    @classmethod
    def genie_data(cls, full_data_set) -> None:
        """
        Global tables in <modpack>/data/util/xr/genie.nyan (GenieData): player
        resources per civ, terrain restrictions, Genie terrain IDs and tech UI data.
        Must run after the nyan objects of terrains, techs and civs exist.
        """
        api_objects = full_data_set.nyan_api_objects
        group = ConverterObjectGroup(-2)

        terrain_lookup_dict = internal_name_lookups.get_terrain_lookups(full_data_set.game_version)
        civ_lookup_dict = internal_name_lookups.get_civ_lookups(full_data_set.game_version)
        tech_lookup_dict = internal_name_lookups.get_tech_lookups(full_data_set.game_version)

        data_ref = "util.xr.genie.GenieData"
        data_raw_api_object = RawAPIObject(data_ref, "GenieData", api_objects, "data/util/xr/")
        data_raw_api_object.set_filename("genie")
        data_raw_api_object.add_raw_parent("engine.xr.util.GenieData")
        group.add_raw_api_object(data_raw_api_object)
        origin = "engine.xr.util.GenieData"

        # Player resources per civ (index = Genie resource ID)
        civs = []
        for civ_id, civ in sorted(full_data_set.genie_civs.items()):
            civ_ref = f"{data_ref}.Civ{civ_id}"
            civ_raw_api_object = RawAPIObject(civ_ref, f"Civ{civ_id}", api_objects)
            civ_raw_api_object.add_raw_parent("engine.xr.util.GenieCiv")
            civ_raw_api_object.set_location(ForwardRef(group, data_ref))
            civ_origin = "engine.xr.util.GenieCiv"
            civ_raw_api_object.add_raw_member("civ_id", civ_id, civ_origin)

            setup = MemberSpecialValue.NYAN_NONE
            civ_group = full_data_set.civ_groups.get(civ_id)
            if civ_group is not None and civ_id in civ_lookup_dict:
                setup_ref = civ_lookup_dict[civ_id][0]
                if civ_group.has_raw_api_object(setup_ref):
                    setup = ForwardRef(civ_group, setup_ref)

            civ_raw_api_object.add_raw_member("setup", setup, civ_origin)

            resources = {}
            for index, member in enumerate(cls._value(civ, "resources", [])):
                resources[index] = float(member.value)

            civ_raw_api_object.add_raw_member("resources", resources, civ_origin)
            group.add_raw_api_object(civ_raw_api_object)
            civs.append(ForwardRef(group, civ_ref))

        data_raw_api_object.add_raw_member("civs", civs, origin)

        # Genie terrain ID -> converted terrain
        terrains = {}
        for terrain_id, terrain_group in sorted(full_data_set.terrain_groups.items()):
            if terrain_id not in terrain_lookup_dict:
                continue

            terrain_ref = terrain_lookup_dict[terrain_id][1]
            if terrain_group.has_raw_api_object(terrain_ref):
                terrains[terrain_id] = ForwardRef(terrain_group, terrain_ref)

        data_raw_api_object.add_raw_member("terrains", terrains, origin)

        # Terrain restrictions: factor per Genie terrain ID (pass > 0, build > 0.05, damage 0 = 1)
        restrictions = []
        for restriction_id, restriction in sorted(full_data_set.genie_terrain_restrictions.items()):
            restriction_ref = f"{data_ref}.TerrainRestriction{restriction_id}"
            restriction_raw_api_object = RawAPIObject(
                restriction_ref, f"TerrainRestriction{restriction_id}", api_objects
            )
            restriction_raw_api_object.add_raw_parent("engine.xr.util.GenieTerrainRestriction")
            restriction_raw_api_object.set_location(ForwardRef(group, data_ref))
            restriction_origin = "engine.xr.util.GenieTerrainRestriction"
            restriction_raw_api_object.add_raw_member("restriction_id", restriction_id, restriction_origin)

            factors = {}
            for terrain_id, member in enumerate(cls._value(restriction, "accessible_dmgmultiplier", [])):
                factors[terrain_id] = float(member.value)

            restriction_raw_api_object.add_raw_member("factors", factors, restriction_origin)
            group.add_raw_api_object(restriction_raw_api_object)
            restrictions.append(ForwardRef(group, restriction_ref))

        data_raw_api_object.add_raw_member("terrain_restrictions", restrictions, origin)

        # Tech interface data (buttons, hotkeys, strings); tech = converted Tech if any
        techs = []
        for tech_id, genie_tech in sorted(full_data_set.genie_techs.items()):
            tech_ref = f"{data_ref}.Tech{tech_id}"
            tech_raw_api_object = RawAPIObject(tech_ref, f"Tech{tech_id}", api_objects)
            tech_raw_api_object.add_raw_parent("engine.xr.util.GenieTech")
            tech_raw_api_object.set_location(ForwardRef(group, data_ref))
            tech_origin = "engine.xr.util.GenieTech"
            tech_raw_api_object.add_raw_member("research_id", tech_id, tech_origin)

            tech = MemberSpecialValue.NYAN_NONE
            tech_group = full_data_set.tech_groups.get(tech_id)
            if tech_group is not None and tech_id in tech_lookup_dict:
                tech_name = tech_lookup_dict[tech_id][0]
                if tech_group.has_raw_api_object(tech_name):
                    tech = ForwardRef(tech_group, tech_name)

            tech_raw_api_object.add_raw_member("tech", tech, tech_origin)
            cls._add_fields(tech_raw_api_object, genie_tech, XR_GENIE_TECH_FIELDS, tech_origin)

            required = []
            for member in cls._value(genie_tech, "required_techs", []):
                if member.value > -1 and member.value not in required:
                    required.append(member.value)

            tech_raw_api_object.add_raw_member("required_techs", required, tech_origin)
            group.add_raw_api_object(tech_raw_api_object)
            techs.append(ForwardRef(group, tech_ref))

        data_raw_api_object.add_raw_member("techs", techs, origin)

        group.create_nyan_objects()
        group.create_nyan_members()
        group.check_readiness()

        # exported with the pregenerated objects (AoCModpackSubprocessor.organize_nyan_objects)
        for raw_api_object in group.get_raw_api_objects().values():
            full_data_set.pregen_nyan_objects.update({raw_api_object.get_id(): raw_api_object})
