# Copyright 2021-2023 the openage authors. See copying.md for legal info.
#
# pylint: disable=too-many-locals,too-many-statements

"""
Convert media information to metadata definitions and export
requests. Subroutine of the main HD processor.
"""

from __future__ import annotations

import typing

from ....entity_object.export.formats.terrain_metadata import LayerMode as TerrainLayerMode
from ....entity_object.export.media_export_request import MediaExportRequest
from ....entity_object.export.metadata_export import (
    TerrainMetadataExport,
    TextureMetadataExport,
)
from ....value_object.read.media_types import MediaType
from ..aoc.media_subprocessor import AoCMediaSubprocessor

if typing.TYPE_CHECKING:
    from openage.convert.entity_object.conversion.aoc.genie_object_container import GenieObjectContainer


class HDMediaSubprocessor:
    """
    Creates the exports requests for media files from HD Edition.
    """

    @classmethod
    def convert(cls, full_data_set: GenieObjectContainer) -> None:
        """
        Create all export requests for the dataset.
        """
        cls.create_graphics_requests(full_data_set)
        cls.create_sound_requests(full_data_set)

    @staticmethod
    def create_graphics_requests(full_data_set: GenieObjectContainer) -> None:
        """
        Create export requests for graphics referenced by CombinedSprite objects.
        """
        AoCMediaSubprocessor.create_sprite_requests(full_data_set)

        combined_terrains = full_data_set.combined_terrains.values()
        for texture in combined_terrains:
            srcfile_prefix = texture.get_terrain()["filename"].value

            targetdir = texture.resolve_graphics_location()
            source_filename = f"{srcfile_prefix!s}_00_color.png"
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
                        "size": (512, 512),
                        "subtex_metadata": [
                            {
                                "x": 0,
                                "y": 0,
                                "w": 512,
                                "h": 512,
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
