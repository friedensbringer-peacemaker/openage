# Copyright 2020-2022 the openage authors. See copying.md for legal info.

"""
References a graphic in the game that has to be converted.
"""

from __future__ import annotations

import posixpath
import typing

if typing.TYPE_CHECKING:
    from openage.convert.entity_object.conversion.aoc.genie_graphic import GenieGraphic
    from openage.convert.entity_object.conversion.aoc.genie_object_container import GenieObjectContainer
    from openage.convert.entity_object.conversion.converter_object import (
        ConverterObject,
        RawAPIObject,
    )


def relative_to_referer(target_path: str, referer_dir: str) -> str:
    """
    Return the modpack path target_path relative to the directory referer_dir
    (both relative to the modpack root), in the "./" notation of nyan files.
    """
    path = posixpath.relpath(target_path, referer_dir)
    if not path.startswith("../"):
        path = f"./{path}"

    return path


class CombinedSprite:
    """
    Collection of sprite information for openage files.

    This will become a spritesheet texture with a sprite file.
    """

    __slots__ = ("_refs", "data", "filename", "head_sprite_id", "metadata")

    def __init__(self, head_sprite_id: int, filename: str, full_data_set: GenieObjectContainer):
        """
        Creates a new CombinedSprite instance.

        :param head_sprite_id: The id of the top level graphic of this sprite.
        :type head_sprite_id: int
        :param filename: Name of the sprite and definition file.
        :type filename: str
        :param full_data_set: ConverterObjectContainer instance that
                              contains all relevant data for the conversion
                              process.
        :type full_data_set: class: ...dataformat.converter_object.ConverterObjectContainer
        """

        self.head_sprite_id = head_sprite_id
        self.filename = filename
        self.data = full_data_set

        self.metadata = None

        # Depending on the amounts of references:
        # 0 = do not convert;
        # 1 = store with GameEntity;
        # >1 = store in 'shared' resources;
        self._refs = []

    def add_reference(self, referer: RawAPIObject) -> None:
        """
        Add an object that is referencing this sprite.
        """
        self._refs.append(referer)

    def get_filename(self) -> str:
        """
        Returns the desired filename of the sprite.
        """
        return self.filename

    def get_graphics(self) -> list[GenieGraphic]:
        """
        Return all graphics referenced by this sprite.
        """
        graphics = [self.data.genie_graphics[self.head_sprite_id]]
        graphics.extend(self.data.genie_graphics[self.head_sprite_id].get_subgraphics())

        # Only consider existing graphics
        existing_graphics = []
        for graphic in graphics:
            if graphic.exists:
                existing_graphics.append(graphic)

        return existing_graphics

    def get_id(self) -> int:
        """
        Returns the head sprite ID of the sprite.
        """
        return self.head_sprite_id

    def get_relative_sprite_location(self, referer_dir: str | None = None) -> str | None:
        """
        Return the sprite file location relative to where the file
        is expected to be in the modpack.

        :param referer_dir: Directory of the nyan file that references the sprite
                            (relative to the modpack root). Shared sprites are
                            stored in data/game_entity/shared/graphics/, which is
                            not "../shared/graphics/" from data/game_entity/<x>/<y>/,
                            so the path has to be computed from the referer.
        """
        if referer_dir is not None and len(self._refs) > 0:
            return relative_to_referer(f"{self.resolve_sprite_location()}{self.filename}.sprite", referer_dir)

        if len(self._refs) > 1:
            return f"../shared/graphics/{self.filename}.sprite"

        if len(self._refs) == 1:
            return f"./graphics/{self.filename}.sprite"

        return None

    def remove_reference(self, referer: ConverterObject) -> None:
        """
        Remove an object that is referencing this sprite.
        """
        self._refs.remove(referer)

    def resolve_graphics_location(self) -> dict[int, str]:
        """
        Returns the planned location in the modpack of all image files
        referenced by the sprite.
        """
        location_dict: dict[int, str] = {}

        for graphic in self.get_graphics():
            if graphic.is_shared():
                location_dict.update({graphic.get_id(): "data/game_entity/shared/graphics/"})

            else:
                location_dict.update({graphic.get_id(): self.resolve_sprite_location()})

        return location_dict

    def resolve_sprite_location(self) -> str:
        """
        Returns the planned location of the definition file in the modpack.
        """
        if len(self._refs) > 1:
            return "data/game_entity/shared/graphics/"

        if len(self._refs) == 1:
            return f"{self._refs[0].get_file_location()[0]}{'graphics/'}"

        raise ValueError(f"{self!r}: sprite has no referencing objects")

    def __repr__(self):
        return f"CombinedSprite<{self.head_sprite_id}>"
