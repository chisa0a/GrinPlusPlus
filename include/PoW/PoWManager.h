#pragma once

//
// This code is free for all purposes without any express guarantee it works.
//
// Author: David Burkett (davidburkett38@gmail.com)
//

#include <Common/ImportExport.h>
#include <Config/Config.h>
#include <Core/Models/BlockHeader.h>
#include <Database/BlockDb.h>

#ifdef MW_POW
#define POW_API EXPORT
#else
#define POW_API IMPORT
#endif

class POW_API PoWManager
{
public:
	PoWManager(const Config& config, const IBlockDB& blockDB);
	~PoWManager() = default;

	bool IsPoWValid(const BlockHeader& header, const BlockHeader& previousHeader) const;

private:
	const Config& m_config;
	const IBlockDB& m_blockDB;
};