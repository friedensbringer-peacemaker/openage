# Copyright 2019-2023 the openage authors. See copying.md for legal info.
#
# pylint: disable=too-many-locals,too-few-public-methods,too-many-statements
"""
Convert media information to metadata definitions and export
requests. Subroutine of the main AoC processor.
"""

from __future__ import annotations

import posixpath
import typing

from ....entity_object.export.formats.sprite_metadata import LayerMode as SpriteLayerMode
from ....entity_object.export.formats.terrain_metadata import LayerMode as TerrainLayerMode
from ....entity_object.export.media_export_request import MediaExportRequest
from ....entity_object.export.metadata_export import (
    SpriteMetadataExport,
    TerrainMetadataExport,
    TextureMetadataExport,
)
from ....value_object.read.media_types import MediaType

if typing.TYPE_CHECKING:
    from openage.convert.entity_object.conversion.aoc.genie_object_container import GenieObjectContainer


class AoCMediaSubprocessor:
    """
    Creates the exports requests for media files from AoC.
    """

    @classmethod
    def convert(cls, full_data_set: GenieObjectContainer) -> None:
        """
        Create all export requests for the dataset.
        """
        cls.create_graphics_requests(full_data_set)
        # cls.create_blend_requests(full_data_set)
        cls.create_sound_requests(full_data_set)

    @staticmethod
    def create_sprite_requests(full_data_set: GenieObjectContainer) -> None:
        """
        Create export requests and sprite/texture metadata for graphics
        referenced by CombinedSprite objects.

        A graphic (SLP) is exported only once, even if several sprites use it
        (e.g. the annexes of a building that appear in its idle and damage
        sprites, or deltas shared by several civ variants). Every sprite still
        gets a layer for each of its graphics. The texture reference in the
        sprite file is relative to the sprite's own directory, because shared
        graphics are stored in a different folder than the sprite.
        """
        combined_sprites = full_data_set.combined_sprites.values()

        # graphic ID -> (export request, image filename, texture metadata path in the modpack)
        handled_graphics: dict[int, tuple[MediaExportRequest, str, str]] = {}

        for sprite in combined_sprites:
            ref_graphics = sprite.get_graphics()
            graphic_targetdirs = sprite.resolve_graphics_location()
            sprite_dir = sprite.resolve_sprite_location()

            # Animation metadata file definiton
            sprite_meta_filename = f"{sprite.get_filename()}.sprite"
            sprite_meta_export = SpriteMetadataExport(sprite_dir, sprite_meta_filename)
            full_data_set.metadata_exports.append(sprite_meta_export)

            sprite_graphic_ids = set()
            for graphic in ref_graphics:
                graphic_id = graphic.get_id()
                if graphic_id in sprite_graphic_ids:
                    continue

                sprite_graphic_ids.add(graphic_id)

                if graphic_id not in handled_graphics:
                    # Texture image file definiton
                    targetdir = graphic_targetdirs[graphic_id]
                    source_filename = f"{graphic['slp_id'].value!s}.slp"
                    target_filename = f"{sprite.get_filename()}_{graphic['slp_id'].value!s}.png"

                    export_request = MediaExportRequest(
                        MediaType.GRAPHICS, targetdir, source_filename, target_filename
                    )
                    full_data_set.graphics_exports.update({graphic_id: export_request})

                    # Texture metadata file definiton
                    # Same file stem as the image file and same targetdir
                    texture_meta_filename = f"{target_filename[:-4]}.texture"
                    texture_meta_export = TextureMetadataExport(targetdir, texture_meta_filename)
                    full_data_set.metadata_exports.append(texture_meta_export)

                    # Add texture image filename to texture metadata
                    texture_meta_export.add_imagefile(target_filename)
                    export_request.add_observer(texture_meta_export)

                    handled_graphics[graphic_id] = (
                        export_request,
                        target_filename,
                        f"{targetdir}{texture_meta_filename}",
                    )

                export_request, target_filename, texture_meta_path = handled_graphics[graphic_id]

                # Add metadata from graphics to animation metadata
                sequence_type = graphic["sequence_type"].value
                if sequence_type == 0x00:
                    layer_mode = SpriteLayerMode.OFF

                elif sequence_type & 0x08:
                    layer_mode = SpriteLayerMode.ONCE

                else:
                    layer_mode = SpriteLayerMode.LOOP

                layer_pos = graphic["layer"].value
                frame_rate = round(graphic["frame_rate"].value, ndigits=6)
                if frame_rate < 0.000001:
                    frame_rate = None

                replay_delay = round(graphic["replay_delay"].value, ndigits=6)
                if replay_delay < 0.000001:
                    replay_delay = None

                frame_count = graphic["frame_count"].value
                angle_count = graphic["angle_count"].value
                mirror_mode = graphic["mirroring_mode"].value
                sprite_meta_export.add_graphics_metadata(
                    target_filename,
                    posixpath.relpath(texture_meta_path, sprite_dir),
                    layer_mode,
                    layer_pos,
                    frame_rate,
                    replay_delay,
                    frame_count,
                    angle_count,
                    mirror_mode,
                )

                # Notify metadata export about SLP metadata when the file is exported
                export_request.add_observer(sprite_meta_export)

    @staticmethod
    def create_graphics_requests(full_data_set: GenieObjectContainer) -> None:
        """
        Create export requests for graphics referenced by CombinedSprite objects.
        """
        AoCMediaSubprocessor.create_sprite_requests(full_data_set)

        combined_terrains = full_data_set.combined_terrains.values()
        for texture in combined_terrains:
            slp_id = texture.get_terrain()["slp_id"].value

            targetdir = texture.resolve_graphics_location()
            source_filename = f"{slp_id!s}.slp"
            target_filename = f"{texture.get_filename()}.png"

            export_request = MediaExportRequest(
                MediaType.TERRAIN, targetdir, source_filename, target_filename
            )
            # Key by terrain, not by SLP ID: several terrains can share an SLP ID
            # and terrain SLP IDs can collide with graphic IDs of sprites. A
            # colliding key silently drops the other export request.
            full_data_set.graphics_exports.update({f"terrain_{texture.get_id()}": export_request})

            texture_meta_filename = f"{texture.get_filename()}.texture"
            texture_meta_export = TextureMetadataExport(targetdir, texture_meta_filename)
            full_data_set.metadata_exports.append(texture_meta_export)

            # Add texture image filename to texture metadata
            texture_meta_export.add_imagefile(target_filename)
            texture_meta_export.update(
                None,
                {
                    f"{target_filename}": {
                        "size": (481, 481),  # TODO: Get actual size = sqrt(slp_frame_count)
                        "subtex_metadata": [
                            {
                                "x": 0,
                                "y": 0,
                                "w": 481,
                                "h": 481,
                                "cx": 0,
                                "cy": 0,
                            }
                        ],
                    }
                },
            )

            terrain_meta_filename = f"{texture.get_filename()}.terrain"
            terrain_meta_export = TerrainMetadataExport(targetdir, terrain_meta_filename)
            full_data_set.metadata_exports.append(terrain_meta_export)

            terrain_meta_export.add_graphics_metadata(
                target_filename, texture_meta_filename, TerrainLayerMode.OFF, 0, 0.0, 0.0, 1
            )

    @staticmethod
    def create_blend_requests(full_data_set: GenieObjectContainer) -> None:
        """
        Create export requests for Blendomatic objects.

        TODO: Blendomatic contains multiple files. Better handling?
        """
        export_request = MediaExportRequest(
            MediaType.BLEND,
            "data/blend/",
            full_data_set.game_version.edition.media_paths[MediaType.BLEND][0],
            "blendmode",
        )
        full_data_set.blend_exports.update({0: export_request})

    @staticmethod
    def create_sound_requests(full_data_set: GenieObjectContainer) -> None:
        """
        Create export requests for sounds referenced by CombinedSound objects.
        """
        combined_sounds = full_data_set.combined_sounds.values()

        for sound in combined_sounds:
            sound_id = sound.get_file_id()

            targetdir = sound.resolve_sound_location()
            source_filename = f"{sound_id!s}.wav"
            target_filename = f"{sound.get_filename()}.opus"

            export_request = MediaExportRequest(MediaType.SOUNDS, targetdir, source_filename, target_filename)

            full_data_set.sound_exports.update({sound_id: export_request})
