#include "SyncVisualizer.h"
#include "GameObject.h"
#include "TimeSyncMgr.h"

void SyncVisualizer::ShowDrift(Player* player, float drift, float latency)
{
    GameObject* marker = player->SummonGameObject(
        50030, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 3.0f, 
        0, 0, 0, 0, 0, 
        10
    );
    if (marker) {
        marker->SetName(fmt::format("Drift: {:.1f}ms Latency: {:.1f}ms", drift, latency));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void SyncVisualizer::ShowDesync(Player* player, float desyncLevel)
{
    GameObject* marker = player->SummonGameObject(
        50031, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 4.0f, 
        0, 0, 0, 0, 0, 
        15
    );
    if (marker) {
        marker->SetName(fmt::format("Desync: {:.1f}%", desyncLevel * 100.0f));
        marker->SetVisibility(VISIBILITY_GM);
    }
}

void SyncVisualizer::ShowTimeSkip(Player* player, uint32 skippedTime)
{
    GameObject* marker = player->SummonGameObject(
        50032, 
        player->GetPositionX(), 
        player->GetPositionY(), 
        player->GetPositionZ() + 5.0f, 
        0, 0, 0, 0, 0, 
        20
    );
    if (marker) {
        marker->SetName(fmt::format("Time Skip: {}ms", skippedTime));
        marker->SetVisibility(VISIBILITY_GM);
    }
}
