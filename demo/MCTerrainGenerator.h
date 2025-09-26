#pragma once

#include <memory>

class MCTilemap;

class MCTerrainGenerator
{
public:
	void Generate(std::shared_ptr<MCTilemap> tilemap);
};

