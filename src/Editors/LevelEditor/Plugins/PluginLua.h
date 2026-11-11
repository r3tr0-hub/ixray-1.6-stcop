#pragma once
#include "PluginManager.h"

class CPluginLua :
	public IPluginBase
{
public:
	CPluginLua();
	virtual void Run() override;
	bool RunChecked(const xr_map<xr_string, xr_string>& Arguments);

private:
	virtual xr_string ReadDesc() const;
};