/*
 *  Copyright (c) 2010,
 *  Gavriloaie Eugen-Andrei (shiretu@gmail.com)
 *
 *  This file is part of crtmpserver.
 *  crtmpserver is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  crtmpserver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with crtmpserver.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _LUAAPI_GENERICS_H
#define	_LUAAPI_GENERICS_H

#include "common.h"

namespace app_vmapp {
	int luaapi_generics_listFolder(lua_State *L);
	int luaapi_generics_normalizePath(lua_State *L);
	int luaapi_generics_splitFileName(lua_State *L);
};

#endif	/* _LUAAPI_GENERICS_H */

