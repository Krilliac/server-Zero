/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2025 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

#include "PositionCorrector.h"
#include "Player.h"
#include "Log.h"
#include "Config/Config.h"

Position PositionCorrector::Reconcile(Player* player, const Position& serverPos, const Position& clientPos)
{
    // Get configurable values
    float blendFactor = sConfig.GetFloatDefault("Movement.CorrectionFactor", BLEND_FACTOR);
    float maxError = sConfig.GetFloatDefault("Movement.MaxPositionError", MAX_ERROR);
    
    float dist = serverPos.GetExactDist(clientPos);
    
    if (dist > maxError)
    {
        Position corrected;
        // Linear interpolation: corrected = clientPos * (1 - blendFactor) + serverPos * blendFactor
        corrected.x = clientPos.x * (1 - blendFactor) + serverPos.x * blendFactor;
        corrected.y = clientPos.y * (1 - blendFactor) + serverPos.y * blendFactor;
        corrected.z = clientPos.z * (1 - blendFactor) + serverPos.z * blendFactor;
        
        if (sConfig.GetBoolDefault("Movement.LogCorrections", false)) {
            sLog.outDebug("PositionCorrector: Adjusted %s (%.2fyd error)", 
                         player->GetName(), dist);
        }
        return corrected;
    }
    return clientPos;
}