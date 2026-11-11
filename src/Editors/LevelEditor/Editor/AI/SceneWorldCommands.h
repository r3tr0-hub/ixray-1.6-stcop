#pragma once

namespace AiWorld
{
static bool IsName(const xr_string& Name)
{
	return !Name.empty() && Name.size() <= 128 && Name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == xr_string::npos;
}

static bool IsEditable(const CCustomObject* Object, bool GroupMembership = false)
{
	return !Object->Locked() && !Object->RegionDisabled() && (GroupMembership || Object->Editable()) &&
		Object->FParentTools->IsEditable() && Object->FParentTools->IsEnabled();
}

static void SetPage(AiJson& Result, const char* Key, AiJson Items, size_t Total, size_t Offset)
{
	const size_t Next = std::min(Offset, Total) + Items.size();
	Result[Key] = std::move(Items);
	Result["total"] = Total;
	Result["next_offset"] = Next;
	Result["has_more"] = Next < Total;
}

static AiJson PartitionState()
{
	return {
		{"ok", true},
		{"enabled", GWorldPartition.IsEnabled()},
		{"cell_size_m", GWorldPartition.GetCellSize()},
		{"stream_around_camera", GWorldPartition.StreamAroundCamera},
		{"stream_radius_m", GWorldPartition.StreamRadius},
		{"pending", GWorldPartition.GetPendingCount()}
	};
}

static AiJson PointData(const CWayPoint* Point)
{
	AiJson Links = AiJson::array();
	for (const SWPLink* Link : Point->GetLinks())
	{
		Links.push_back({{"target", AiText::Utf8(Link->way_point->GetName()).c_str()}, {"probability", Link->probability}});
	}
	return {
		{"name", AiText::Utf8(Point->GetName()).c_str()},
		{"position_m", AiVectorJson(Point->GetPosition())},
		{"flags", Point->GetFlags()},
		{"links", std::move(Links)}
	};
}

static CWayPoint* FindPoint(const CWayObject* Way, const xr_string& Name)
{
	if (!Name.empty())
	{
		for (CWayPoint* Point : Way->GetPoints())
		{
			if (AiText::Utf8(Point->GetName()) == Name)
			{
				return Point;
			}
		}
	}
	return nullptr;
}

// A portal is a small, ordered convex polygon. Every vertex must be on the
// same plane and strictly on the inner side of each nonincident edge.
static bool ReadPortalVertices(const AiJson& Params, FvectorVec& Vertices)
{
	const auto Points = Params.find("vertices_m");
	if (Points == Params.end() || !Points->is_array() || Points->size() < 3 || Points->size() > XR_MAX_PORTAL_VERTS)
	{
		return false;
	}
	for (const auto& Point : *Points)
	{
		Fvector Position;
		if (!AiVector(AiJson{{"vertex", Point}}, "vertex", Position, 0, 1000000))
		{
			return false;
		}
		Vertices.push_back(Position);
	}

	Fvector FirstEdge, SecondEdge, Normal;
	FirstEdge.sub(Vertices[1], Vertices[0]);
	SecondEdge.sub(Vertices[2], Vertices[0]);
	Normal.crossproduct(FirstEdge, SecondEdge);
	if (Normal.magnitude() < 0.0001f)
	{
		return false;
	}
	Normal.normalize();

	for (size_t Index = 0; Index < Vertices.size(); ++Index)
	{
		Fvector Offset;
		Offset.sub(Vertices[Index], Vertices[0]);
		if (std::abs(Normal.dotproduct(Offset)) > 0.001f)
		{
			return false;
		}

		const size_t Next = (Index + 1) % Vertices.size();
		Fvector Edge;
		Edge.sub(Vertices[Next], Vertices[Index]);
		if (Edge.magnitude() < 0.001f)
		{
			return false;
		}
		for (size_t Other = 0; Other < Vertices.size(); ++Other)
		{
			if (Other == Index || Other == Next)
			{
				continue;
			}
			Fvector FromEdge, Side;
			FromEdge.sub(Vertices[Other], Vertices[Index]);
			Side.crossproduct(Edge, FromEdge);
			if (Side.dotproduct(Normal) < 0.00001f)
			{
				return false;
			}
		}
	}
	return true;
}
}

inline AiSceneExecutor::Json AiSceneExecutor::DispatchWorld(const xr_string& Method, const Json& Params)
{
	if (Method == "scene.partition_read")
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit))
		{
			return Error("INVALID_PAGE", "Provide valid pagination");
		}

		const auto& Cells = GWorldPartition.GetCells();
		xr_vector<u64> Keys;
		Keys.reserve(Cells.size());
		for (const auto& [Key, Cell] : Cells)
		{
			Keys.push_back(Key);
		}
		std::sort(Keys.begin(), Keys.end(), [](u64 Left, u64 Right)
		{
			return std::pair(CWorldPartition::KeyX(Left), CWorldPartition::KeyZ(Left)) <
				std::pair(CWorldPartition::KeyX(Right), CWorldPartition::KeyZ(Right));
		});

		Json Items = Json::array();
		const size_t End = std::min(Offset + Limit, Keys.size());
		for (size_t Index = std::min(Offset, End); Index < End; ++Index)
		{
			const u64 Key = Keys[Index];
			const auto& Cell = Cells.at(Key);
			Items.push_back({
				{"cell", {CWorldPartition::KeyX(Key), CWorldPartition::KeyZ(Key)}},
				{"objects", Cell.Objects},
				{"unloaded", Cell.Unloaded},
				{"disabled", GWorldPartition.IsCellDisabled(Key)},
				{"outside_stream_radius", GWorldPartition.IsCellStreamedOut(Key)}
			});
		}
		Json Result = AiWorld::PartitionState();
		AiWorld::SetPage(Result, "cells", std::move(Items), Keys.size(), Offset);
		return Result;
	}

	const xr_string Action = AiString(Params, "action");
	if (Method == "scene.ai_map_read" || Method == "scene.ai_map_edit")
	{
		auto* Map = smart_cast<ESceneAIMapTool*>(Scene->GetTool(OBJCLASS_AIMAP));
		if (Map == nullptr || !Map->IsEnabled())
		{
			return Error("AI_MAP_UNAVAILABLE", "Enable the AI Map tool in the Level Editor");
		}
		auto& Nodes = Map->Nodes();
		auto State = [Map]()
		{
			Json Snap = Json::array();
			for (CCustomObject* Object : *Map->GetSnapList())
			{
				if (Object != nullptr)
				{
					Snap.push_back(AiText::Utf8(Object->GetName()).c_str());
				}
			}
			return Json{
				{"ok", true},
				{"map_revision", Scene->GetModifyCount()},
				{"node_count", Map->Nodes().size()},
				{"snap_objects", std::move(Snap)},
				{"patch_size_m", Map->AIParams().fPatchSize}
			};
		};
		if (Method == "scene.ai_map_read")
		{
			size_t Offset, Limit;
			if (!AiPage(Params, Offset, Limit))
			{
				return Error("INVALID_PAGE", "Provide valid pagination");
			}
			xr_hash_map<const SAINode*, size_t> Indices;
			Indices.reserve(Nodes.size());
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				if (Nodes[Index] != nullptr)
				{
					Indices.emplace(Nodes[Index], Index);
				}
			}
			Json Items = Json::array();
			const size_t End = std::min(Offset + Limit, Nodes.size());
			for (size_t Index = std::min(Offset, End); Index < End; ++Index)
			{
				const SAINode* Node = Nodes[Index];
				if (Node == nullptr)
				{
					Items.push_back({{"index", Index}, {"invalid", true}});
					continue;
				}
				Json Links = Json::array();
				for (const SAINode* Neighbor : Node->n)
				{
					const auto It = Indices.find(Neighbor);
					Links.push_back(It == Indices.end() ? Json() : Json(It->second));
				}
				Items.push_back({
					{"index", Index},
					{"position_m", AiVectorJson(Node->Pos)},
					{"selected", Node->flags.is(SAINode::flSelected)},
					{"neighbors_left_forward_right_backward", std::move(Links)}
				});
			}
			Json Result = State();
			AiWorld::SetPage(Result, "nodes", std::move(Items), Nodes.size(), Offset);
			return Result;
		}

		if (!Map->IsEditable())
		{
			return Error("TOOL_NOT_EDITABLE", "The AI Map tool is locked");
		}
		const auto Revision = Params.find("map_revision");
		if (Revision == Params.end() || !Revision->is_number_integer() || Revision->get<double>() != Scene->GetModifyCount())
		{
			return Error("STALE_AI_MAP", "Read the AI map and send its current map_revision");
		}

		const bool Scoped = Action == "remove_nodes" || Action == "set_height" || Action == "make_links" || Action == "remove_links";
		const bool ExplicitSeeds = Action == "generate" && Params.contains("nodes");
		xr_hash_set<SAINode*> Targets;
		if (Scoped || ExplicitSeeds)
		{
			const auto Indices = Params.find("nodes");
			if (Indices == Params.end() || !Indices->is_array() || Indices->empty() || Indices->size() > 128)
			{
				return Error("INVALID_AI_NODES", "Specify 1 to 128 current node indices");
			}
			for (const Json& Index : *Indices)
			{
				if (!Index.is_number_integer() || Index.get<double>() < 0 || Index.get<double>() >= Nodes.size())
				{
					return Error("UNKNOWN_AI_NODE", "A node index is not in the current AI map");
				}
				SAINode* Node = Nodes[Index.get<size_t>()];
				if (Node == nullptr)
				{
					return Error("INVALID_AI_NODE", "Run cleanup to remove invalid nodes");
				}
				Targets.insert(Node);
			}
		}

		if (Action == "set_snap")
		{
			const auto Ids = Params.find("ids");
			if (Ids == Params.end() || !Ids->is_array() || Ids->empty())
			{
				return Error("INVALID_IDS", "Specify geometry object IDs for the AI Map snap list");
			}
			xr_vector<CCustomObject*> Geometry;
			Json Failure;
			if (!Resolve(Json{{"ids", *Ids}}, Geometry, Failure, false))
			{
				return Failure;
			}
			ObjectList Snap;
			for (CCustomObject* Object : Geometry)
			{
				CEditableObject* Reference = nullptr;
				if (auto* Static = smart_cast<CSceneObject*>(Object))
				{
					if (Static->IsStatic() || Static->IsMUStatic())
					{
						Reference = Static->GetReference();
					}
				}
				else if (auto* Terrain = smart_cast<CTerrain*>(Object))
				{
					Reference = Terrain->GetReference();
				}
				if (Reference == nullptr)
				{
					return Error("INVALID_AI_SNAP_OBJECT", "Use loaded static scene geometry or terrain");
				}
				Snap.push_back(Object);
			}
			g_LevelAutoBackup.Cancel();
			AiSceneSnapshot Before;
			*Map->GetSnapList() = std::move(Snap);
			Map->OnSynchronize();
			Json Result = State();
			FinishChange(Before, Result);
			if (Result.value("ok", false))
			{
				Result["map_revision"] = Scene->GetModifyCount();
			}
			return Result;
		}

		if (Action == "add_node" || Action == "generate")
		{
			if (Map->GetSnapList()->empty())
			{
				return Error("AI_SNAP_LIST_EMPTY", "Set the AI Map snap geometry before adding or generating nodes");
			}
			if (Action == "generate" && Nodes.empty())
			{
				return Error("AI_SEED_REQUIRED", "Add a seed node before generating the AI map");
			}
			for (CCustomObject* Object : *Map->GetSnapList())
			{
				auto* Static = smart_cast<CSceneObject*>(Object);
				auto* Terrain = smart_cast<CTerrain*>(Object);
				if ((Static == nullptr || Static->GetReference() == nullptr) && (Terrain == nullptr || Terrain->GetReference() == nullptr))
				{
					return Error("AI_SNAP_GEOMETRY_UNAVAILABLE", "Load or replace the missing AI Map snap geometry");
				}
			}
		}

		Fvector Position;
		if (Action == "add_node" && (!Params.contains("position_m") || !AiVector(Params, "position_m", Position, 0, 1000000)))
		{
			return Error("INVALID_POSITION", "Provide a finite world position in metres");
		}
		if ((Params.contains("auto_link") && !Params["auto_link"].is_boolean()) ||
			(Params.contains("selected_only") && !Params["selected_only"].is_boolean()))
		{
			return Error("INVALID_AI_MAP_PARAMS", "Use boolean auto_link and selected_only flags");
		}
		const auto Height = Params.find("height_m");
		if (Action == "set_height" && (Height == Params.end() || !Height->is_number() ||
			!std::isfinite(Height->get<double>()) || std::abs(Height->get<double>()) > 1000000))
		{
			return Error("INVALID_HEIGHT", "Provide a finite height in metres");
		}
		if (!Scoped && Action != "add_node" && Action != "generate" && Action != "cleanup")
		{
			return Error("UNKNOWN_ACTION", "Use a supported AI Map action");
		}

		g_LevelAutoBackup.Cancel();
		AiSceneSnapshot Before;
		int RemovedInvalid = 0;
		const size_t PreviousCount = Nodes.size();
		if (Action == "add_node")
		{
			Map->OnSynchronize();
			if (Map->AddNode(Position, false, Params.value("auto_link", true), 1) <= 0)
			{
				return Error("AI_NODE_ADD_FAILED", "The SDK could not place a node on this snap geometry");
			}
		}
		else if (Action == "generate")
		{
			if (ExplicitSeeds)
			{
				for (SAINode* Node : Nodes)
				{
					Node->flags.set(SAINode::flSelected, Targets.contains(Node));
				}
			}
			if (!Map->GenerateMap(ExplicitSeeds || Params.value("selected_only", false), false))
			{
				Before.Restore();
				Synchronize();
				return Error("AI_MAP_GENERATE_FAILED", "The SDK could not generate the AI map");
			}
		}
		else if (Action == "cleanup")
		{
			RemovedInvalid = Map->CleanupInvalidNodes();
		}
		else if (Action == "remove_nodes")
		{
			for (SAINode* Node : Nodes)
			{
				for (size_t Side = 0; Side < 4; ++Side)
				{
					if (Targets.contains(Node->n[Side]))
					{
						Node->n[Side] = nullptr;
					}
				}
			}
			Nodes.erase(std::remove_if(Nodes.begin(), Nodes.end(), [&Targets](SAINode* Node) { return Targets.contains(Node); }), Nodes.end());
			for (SAINode* Node : Targets)
			{
				Map->RemoveNodeImmediate(Node);
			}
			Map->OnSynchronize();
		}
		else if (Action == "set_height")
		{
			for (SAINode* Node : Targets)
			{
				Node->Pos.y = Height->get<float>();
				const Fvector Normal = Node->Plane.n;
				Node->Plane.build(Node->Pos, Normal);
			}
			Map->OnSynchronize();
		}
		else
		{
			xr_vector<bool> PreviousSelection;
			PreviousSelection.reserve(Nodes.size());
			for (SAINode* Node : Nodes)
			{
				PreviousSelection.push_back(Node->flags.is(SAINode::flSelected));
				Node->flags.set(SAINode::flSelected, Targets.contains(Node));
			}
			if (Action == "make_links")
			{
				Map->MakeLinks(SAINode::flN1 | SAINode::flN2 | SAINode::flN3 | SAINode::flN4, ESceneAIMapTool::mdAppend, false);
			}
			else
			{
				Map->RemoveLinks();
			}
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				Nodes[Index]->flags.set(SAINode::flSelected, PreviousSelection[Index]);
			}
		}
		Map->UpdateHLSelected();

		Json Result = State();
		Result["previous_node_count"] = PreviousCount;
		if (Action == "cleanup")
		{
			Result["removed_invalid_count"] = RemovedInvalid;
		}
		if (FinishChange(Before, Result))
		{
			Result["map_revision"] = Scene->GetModifyCount();
		}
		return Result;
	}

	if (Method == "scene.partition_edit")
	{
		if (Action == "enable" || Action == "disable")
		{
			GWorldPartition.SetEnabled(Action == "enable");
		}
		else if (Action == "enable_all")
		{
			g_LevelAutoBackup.Cancel();
			GWorldPartition.EnableAll();
		}
		else if (Action == "set_size")
		{
			const auto Size = Params.find("size_m");
			if (Size == Params.end() || !Size->is_number() || !std::isfinite(Size->get<double>()) ||
				Size->get<double>() < 8 || Size->get<double>() > 100000)
			{
				return Error("INVALID_CELL_SIZE", "Use a finite size between 8 and 100000 metres");
			}
			g_LevelAutoBackup.Cancel();
			GWorldPartition.SetCellSize(Size->get<float>());
		}
		else if (Action == "set_cell" || Action == "focus")
		{
			const auto Cell = Params.find("cell");
			if (Cell == Params.end() || !Cell->is_array() || Cell->size() != 2)
			{
				return Error("INVALID_CELL", "Provide one cell as [x, z]");
			}
			for (const Json& Coordinate : *Cell)
			{
				if (!Coordinate.is_number_integer() || std::abs(Coordinate.get<double>()) > 1000000)
				{
					return Error("INVALID_CELL", "Cell coordinates must be integers within ±1000000");
				}
			}
			const s32 X = (*Cell)[0].get<s32>();
			const s32 Z = (*Cell)[1].get<s32>();
			const u64 Key = CWorldPartition::MakeKey(X, Z);
			if (Action == "focus")
			{
				Fbox Box;
				GWorldPartition.CellBox(X, Z, Box);
				EContext.UI->CurrentView().m_Camera.ZoomExtents(Box);
				EContext.UI->RedrawScene();
			}
			else
			{
				const auto Disabled = Params.find("disabled");
				if (Disabled == Params.end() || !Disabled->is_boolean())
				{
					return Error("INVALID_CELL", "Specify disabled=true or disabled=false");
				}
				g_LevelAutoBackup.Cancel();
				GWorldPartition.SetCellDisabled(Key, Disabled->get<bool>());
				GWorldPartition.Apply();
			}
			Json Result = AiWorld::PartitionState();
			Result["cell"] = Json::array({X, Z});
			Result["disabled"] = GWorldPartition.IsCellDisabled(Key);
			Result["undoable"] = false;
			return Result;
		}
		else
		{
			return Error("UNKNOWN_ACTION", "Use a supported world partition action");
		}
		GWorldPartition.Apply();
		Json Result = AiWorld::PartitionState();
		Result["undoable"] = false;
		return Result;
	}

	const bool IsGroup = Method == "scene.group_read" || Method == "scene.group_edit";
	const bool IsWay = Method == "scene.way_read" || Method == "scene.way_edit";
	const bool IsSector = Method == "scene.sector_read" || Method == "scene.sector_edit";
	const bool IsPortal = Method == "scene.portal_read" || Method == "scene.portal_edit";
	if (!IsGroup && !IsWay && !IsSector && !IsPortal)
	{
		return nullptr;
	}

	const bool Editing = Method.ends_with("_edit");
	const bool Creating = Editing && Action == "create";
	const ObjClassID ClassId = IsGroup ? OBJCLASS_GROUP : IsWay ? OBJCLASS_WAY : IsSector ? OBJCLASS_SECTOR : OBJCLASS_PORTAL;
	CCustomObject* Object = nullptr;
	string256 Name = {};
	if (Creating)
	{
		ESceneCustomOTool* Tool = Scene->GetOTool(ClassId);
		if (Tool == nullptr || !Tool->IsEnabled() || !Tool->IsEditable())
		{
			return Error("TOOL_NOT_EDITABLE", "The SDK object tool is unavailable or locked");
		}
		if (Params.contains("name"))
		{
			const xr_string RequestedName = AiString(Params, "name");
			if (!AiWorld::IsName(RequestedName))
			{
				return Error("INVALID_NAME", "Use 1 to 128 lowercase ASCII letters, digits, hyphens or underscores");
			}
			for (CCustomObject* Current : Objects())
			{
				if (RequestedName == Current->GetName())
				{
					return Error("DUPLICATE_NAME", "An object already uses this name");
				}
			}
			xr_strcpy(Name, RequestedName.c_str());
		}
		else
		{
			Scene->GenObjectName(ClassId, Name);
		}
	}
	else
	{
		if (AiHasString(Params, "object_id", 128))
		{
			Object = Find(AiString(Params, "object_id"));
		}
		if (Object == nullptr || (Object->FClassID != ClassId && !(Method == "scene.sector_read" && Object->FClassID == OBJCLASS_SCENEOBJECT)))
		{
			return Error("INVALID_OBJECT", "Provide the current object ID of the requested class");
		}
		if (Editing && (!AiWorld::IsEditable(Object) || Object->GetOwner() != nullptr))
		{
			return Error("OBJECT_NOT_EDITABLE", "The object is locked, disabled, or a group member");
		}
	}

	if (IsGroup)
	{
		auto* Group = static_cast<CGroupObject*>(Object);
		ObjectList Members;
		if (Group != nullptr)
		{
			Group->GetObjects(Members);
		}
		if (!Editing)
		{
			size_t Offset, Limit;
			if (!AiPage(Params, Offset, Limit))
			{
				return Error("INVALID_PAGE", "Provide valid pagination");
			}
			Json Items = Json::array();
			size_t Index = 0;
			for (CCustomObject* Member : Members)
			{
				if (Index++ >= Offset && Items.size() < Limit)
				{
					Items.push_back(Describe(Member));
				}
			}
			Json Result = {{"ok", true}, {"object", Describe(Group)}};
			AiWorld::SetPage(Result, "members", std::move(Items), Members.size(), Offset);
			return Result;
		}

		if (Creating || Action == "attach" || Action == "detach")
		{
			const auto Ids = Params.find("ids");
			if (Ids == Params.end() || !Ids->is_array() || Ids->empty())
			{
				return Error("INVALID_IDS", "Specify member object IDs explicitly");
			}
			xr_vector<CCustomObject*> Targets;
			Json Failure;
			if (!Resolve(Json{{"ids", *Ids}}, Targets, Failure, false))
			{
				return Failure;
			}
			ObjectList ToChange;
			for (CCustomObject* Target : Targets)
			{
				const bool ValidOwner = Action == "detach" ? Target->GetOwner() == Group : Target->GetOwner() == nullptr;
				if (!ValidOwner || !AiWorld::IsEditable(Target, Action == "detach") || !Target->CanAttach() || Target == Group)
				{
					return Error("INVALID_GROUP_MEMBER", "Every target must be editable and have the expected group membership");
				}
				ToChange.push_back(Target);
			}
			if (Action == "detach" && ToChange.size() >= Members.size())
			{
				return Error("LAST_GROUP_MEMBER", "Use ungroup to release all members");
			}
			AiSceneSnapshot Before;
			if (Creating)
			{
				Group = new CGroupObject(nullptr, Name);
				Group->GroupObjects(ToChange);
				Scene->AppendObject(Group, false);
			}
			else
			{
				Scene->BeforeObjectChange(Group);
				if (Action == "attach")
				{
					Group->AttachObjects(ToChange);
				}
				else
				{
					for (CCustomObject* Target : ToChange)
					{
						Group->DetachObject(Target);
					}
				}
			}
			GWorldPartition.MarkDirty();
			Json Result = {{"object", Describe(Group)}, {"member_count", Group->ObjectInGroupCount()}};
			FinishChange(Before, Result);
			return Result;
		}

		if (Action == "pivot")
		{
			if (Members.empty())
			{
				return Error("EMPTY_GROUP", "The group must have at least one member");
			}
			CCustomObject* Pivot = nullptr;
			if (Params.contains("pivot_id"))
			{
				Pivot = Find(AiString(Params, "pivot_id"));
				if (Pivot == nullptr || Pivot->GetOwner() != Group)
				{
					return Error("INVALID_PIVOT", "The pivot object must be a member of the group");
				}
			}
			AiSceneSnapshot Before;
			Scene->BeforeObjectChange(Group);
			Group->UpdatePivot(Pivot == nullptr ? nullptr : Pivot->GetName(), Pivot == nullptr);
			GWorldPartition.MarkDirty();
			Json Result = {{"object", Describe(Group)}, {"member_count", Members.size()}};
			FinishChange(Before, Result);
			return Result;
		}

		if (Action == "ungroup")
		{
			for (CCustomObject* Member : Members)
			{
				if (!AiWorld::IsEditable(Member, true))
				{
					return Error("OBJECT_NOT_EDITABLE", "A group member is locked or disabled");
				}
			}
			AiSceneSnapshot Before;
			const xr_string RemovedId = Identify(Group);
			Group->UngroupObjects();
			Scene->RemoveObject(Group, false, true);
			ByObject.erase(Group);
			ById.erase(RemovedId);
			xr_delete(Group);
			Json Result = {{"removed_group_id", RemovedId.c_str()}, {"released_count", Members.size()}};
			FinishChange(Before, Result);
			return Result;
		}

		return Error("UNKNOWN_ACTION", "Use a supported group action");
	}

	if (IsWay)
	{
		auto* Way = static_cast<CWayObject*>(Object);
		if (!Editing)
		{
			size_t Offset, Limit;
			if (!AiPage(Params, Offset, Limit))
			{
				return Error("INVALID_PAGE", "Provide valid pagination");
			}
			const auto& Points = Way->GetPoints();
			Json Items = Json::array();
			const size_t End = std::min(Offset + Limit, Points.size());
			for (size_t Index = std::min(Offset, End); Index < End; ++Index)
			{
				Items.push_back(AiWorld::PointData(Points[Index]));
			}
			Json Result = {{"ok", true}, {"object", Describe(Way)}};
			AiWorld::SetPage(Result, "points", std::move(Items), Points.size(), Offset);
			return Result;
		}

		if (Creating || Action == "add_point" || Action == "move_point")
		{
			Fvector Position;
			if (!Params.contains("position_m") || !AiVector(Params, "position_m", Position, 0, 1000000))
			{
				return Error("INVALID_POSITION", "Provide a finite world position in metres");
			}
			CWayPoint* Point = Action == "move_point" ? AiWorld::FindPoint(Way, AiString(Params, "point")) : nullptr;
			if (Action == "move_point" && Point == nullptr)
			{
				return Error("UNKNOWN_POINT", "Provide an exact point name from way_read");
			}
			AiSceneSnapshot Before;
			if (Creating)
			{
				Way = new CWayObject(nullptr, Name);
				Point = Way->GetPoints().front();
				Point->MoveTo(Position);
				Scene->AppendObject(Way, false);
			}
			else
			{
				Scene->BeforeObjectChange(Way);
				if (Point == nullptr)
				{
					Point = Way->AppendWayPoint();
				}
				Point->MoveTo(Position);
				Way->UpdateTransform();
			}
			GWorldPartition.MarkDirty();
			Json Result = {{"object", Describe(Way)}, {"point", AiWorld::PointData(Point)}};
			FinishChange(Before, Result);
			return Result;
		}

		CWayPoint* Point = AiWorld::FindPoint(Way, AiString(Params, "point"));
		if (Point == nullptr)
		{
			return Error("UNKNOWN_POINT", "Provide an exact point name from way_read");
		}
		if (Action == "remove_point")
		{
			if (Way->GetPoints().size() <= 1)
			{
				return Error("LAST_WAY_POINT", "A patrol path must keep at least one point");
			}
			AiSceneSnapshot Before;
			Scene->BeforeObjectChange(Way);
			Way->RemovePoint(Point);
			Way->UpdateTransform();
			GWorldPartition.MarkDirty();
			Json Result = {{"object", Describe(Way)}, {"point_count", Way->GetPoints().size()}};
			FinishChange(Before, Result);
			return Result;
		}

		if (Action == "rename_point")
		{
			const xr_string NewName = AiString(Params, "name");
			if (!AiWorld::IsName(NewName))
			{
				return Error("INVALID_NAME", "Use a lowercase ASCII point name");
			}
			if (AiWorld::FindPoint(Way, NewName) != nullptr)
			{
				return Error("DUPLICATE_NAME", "A path point already uses this name");
			}
			AiSceneSnapshot Before;
			Scene->BeforeObjectChange(Way);
			Point->SetName(NewName.c_str());
			Json Result = {{"object", Describe(Way)}, {"point", AiWorld::PointData(Point)}};
			FinishChange(Before, Result);
			return Result;
		}

		if (Action == "set_flags")
		{
			const auto Flags = Params.find("flags");
			if (Flags == Params.end() || !Flags->is_number_integer() || Flags->get<double>() < 0 || Flags->get<double>() > 4294967295.0)
			{
				return Error("INVALID_FLAGS", "Use an unsigned 32-bit flag mask");
			}
			AiSceneSnapshot Before;
			Scene->BeforeObjectChange(Way);
			Point->SetFlags(Flags->get<u32>());
			Json Result = {{"object", Describe(Way)}, {"point", AiWorld::PointData(Point)}};
			FinishChange(Before, Result);
			return Result;
		}

		if (Action == "link" || Action == "unlink")
		{
			CWayPoint* Target = AiWorld::FindPoint(Way, AiString(Params, "target"));
			const auto Both = Params.find("bidirectional");
			if (Target == nullptr || Target == Point || (Both != Params.end() && !Both->is_boolean()))
			{
				return Error("INVALID_LINK", "Specify two different points on the same path and an optional boolean direction flag");
			}
			const bool Bidirectional = Both != Params.end() && Both->get<bool>();
			const auto Probability = Params.find("probability");
			if (Probability != Params.end() && (Action != "link" || !Probability->is_number() ||
				!std::isfinite(Probability->get<double>()) || Probability->get<double>() < 0 || Probability->get<double>() > 1))
			{
				return Error("INVALID_LINK_PROBABILITY", "Use a link probability from 0 to 1");
			}
			AiSceneSnapshot Before;
			Scene->BeforeObjectChange(Way);
			if (Action == "link")
			{
				if (Bidirectional)
				{
					Point->AddDoubleLink(Target);
				}
				else
				{
					Point->AddSingleLink(Target);
				}
				if (Probability != Params.end())
				{
					(*Point->FindLink(Target))->probability = Probability->get<float>();
					if (Bidirectional)
					{
						(*Target->FindLink(Point))->probability = Probability->get<float>();
					}
				}
			}
			else
			{
				Point->RemoveDirectedLink(Target);
				if (Bidirectional)
				{
					Target->RemoveDirectedLink(Point);
				}
			}
			Json Result = {{"object", Describe(Way)}, {"point", AiWorld::PointData(Point)}, {"target", AiWorld::PointData(Target)}};
			FinishChange(Before, Result);
			return Result;
		}

		return Error("UNKNOWN_ACTION", "Use a supported patrol path action");
	}

	if (Method == "scene.sector_read" && Object->FClassID == OBJCLASS_SCENEOBJECT)
	{
		size_t Offset, Limit;
		if (!AiPage(Params, Offset, Limit))
		{
			return Error("INVALID_PAGE", "Provide valid pagination");
		}
		auto* StaticObject = static_cast<CSceneObject*>(Object);
		const auto* Meshes = StaticObject->Meshes();
		if (Meshes == nullptr)
		{
			return Error("MESH_UNAVAILABLE", "The model is unavailable or streamed out");
		}
		Json Items = Json::array();
		const size_t End = std::min(Offset + Limit, Meshes->size());
		for (size_t Index = std::min(Offset, End); Index < End; ++Index)
		{
			CEditableMesh* Mesh = (*Meshes)[Index];
			const auto* CurrentSector = PortalUtils.FindSector(StaticObject, Mesh);
			Items.push_back({
				{"mesh", AiText::Utf8(Mesh->m_Name.c_str()).c_str()},
				{"assigned_sector", CurrentSector != nullptr}
			});
		}
		Json Result = {{"ok", true}, {"object", Describe(Object)}};
		AiWorld::SetPage(Result, "meshes", std::move(Items), Meshes->size(), Offset);
		return Result;
	}

	if (IsSector)
	{
		auto* Sector = static_cast<CSector*>(Object);
		if (!Editing)
		{
			size_t Offset, Limit;
			if (!AiPage(Params, Offset, Limit))
			{
				return Error("INVALID_PAGE", "Provide valid pagination");
			}
			const auto& Meshes = Sector->GetItems();
			Json Items = Json::array();
			const size_t End = std::min(Offset + Limit, Meshes.size());
			for (size_t Index = std::min(Offset, End); Index < End; ++Index)
			{
				const CSectorItem& Item = Meshes[Index];
				CSceneObject* MeshObject = Item.GetSceneObject();
				Items.push_back({
					{"object_id", MeshObject ? Json(Identify(MeshObject).c_str()) : Json()},
					{"object_name", AiText::Utf8(MeshObject ? MeshObject->GetName() : Item.GetStoredObjectName()).c_str()},
					{"mesh", AiText::Utf8(Item.GetMeshName()).c_str()}
				});
			}
			Json Result = {{"ok", true}, {"object", Describe(Sector)}, {"default", Sector->IsDefault()}};
			AiWorld::SetPage(Result, "meshes", std::move(Items), Meshes.size(), Offset);
			return Result;
		}

		if (Action == "remove")
		{
			for (auto It = Scene->FirstObj(OBJCLASS_PORTAL); It != Scene->LastObj(OBJCLASS_PORTAL); ++It)
			{
				const auto* Portal = static_cast<const CPortal*>(*It);
				if (Portal->SectorFront() == Sector || Portal->SectorBack() == Sector)
				{
					return Error("SECTOR_HAS_PORTALS", "Remove the portals attached to this sector first");
				}
			}
			AiSceneSnapshot Before;
			const xr_string RemovedId = Identify(Sector);
			Scene->RemoveObject(Sector, false, true);
			ByObject.erase(Sector);
			ById.erase(RemovedId);
			xr_delete(Sector);
			Json Result = {{"removed_sector_id", RemovedId.c_str()}};
			FinishChange(Before, Result);
			return Result;
		}

		if (!Creating && Action != "add_mesh" && Action != "remove_mesh")
		{
			return Error("UNKNOWN_ACTION", "Use a supported sector action");
		}
		auto* MeshObject = smart_cast<CSceneObject*>(Find(AiString(Params, "mesh_object_id")));
		if (MeshObject == nullptr || (!MeshObject->IsStatic() && !MeshObject->IsMUStatic()))
		{
			return Error("INVALID_MESH_OBJECT", "Specify a static scene object");
		}
		auto* Meshes = MeshObject->Meshes();
		if (Meshes == nullptr)
		{
			return Error("MESH_UNAVAILABLE", "The object mesh is unavailable or streamed out");
		}
		CEditableMesh* Mesh = nullptr;
		const xr_string MeshName = AiString(Params, "mesh");
		for (CEditableMesh* Current : *Meshes)
		{
			if (AiText::Utf8(Current->m_Name.c_str()) == MeshName)
			{
				Mesh = Current;
				break;
			}
		}
		if (Mesh == nullptr)
		{
			return Error("UNKNOWN_MESH", "Specify the exact mesh name on the static model");
		}
		if (Action == "remove_mesh")
		{
			if (!Sector->Contains(MeshObject, Mesh))
			{
				return Error("MESH_NOT_IN_SECTOR", "The sector does not contain that mesh");
			}
			if (Sector->GetItems().size() <= 1)
			{
				return Error("LAST_SECTOR_MESH", "A sector must keep at least one mesh");
			}
		}
		else if (PortalUtils.FindSector(MeshObject, Mesh) != nullptr)
		{
			return Error("MESH_ALREADY_ASSIGNED", "The mesh already belongs to a sector");
		}
		AiSceneSnapshot Before;
		if (Creating)
		{
			Sector = new CSector(nullptr, Name);
			if (!Sector->AddMesh(MeshObject, Mesh))
			{
				xr_delete(Sector);
				return Error("SDK_COMMAND_FAILED", "The SDK did not assign the mesh");
			}
			Scene->AppendObject(Sector, false);
		}
		else
		{
			Scene->BeforeObjectChange(Sector);
			if (Action == "remove_mesh")
			{
				Sector->DelMesh(MeshObject, Mesh);
			}
			else if (!Sector->AddMesh(MeshObject, Mesh))
			{
				Before.Restore();
				Synchronize();
				return Error("SDK_COMMAND_FAILED", "The SDK did not assign the mesh");
			}
		}
		GWorldPartition.MarkDirty();
		Json Result = {{"object", Describe(Sector)}, {"mesh_count", Sector->GetItems().size()}, {"mesh_present", Sector->Contains(MeshObject, Mesh)}};
		FinishChange(Before, Result);
		return Result;
	}

	auto* Portal = static_cast<CPortal*>(Object);
	auto DescribePortal = [this](CPortal* Current)
	{
		Json Vertices = Json::array();
		for (const Fvector& Vertex : Current->Vertices())
		{
			Vertices.push_back(AiVectorJson(Vertex));
		}
		CSector* Front = Current->SectorFront();
		CSector* Back = Current->SectorBack();
		return Json{
			{"ok", true},
			{"object", Describe(Current)},
			{"front_id", Front ? Json(Identify(Front).c_str()) : Json()},
			{"back_id", Back ? Json(Identify(Back).c_str()) : Json()},
			{"vertices_m", std::move(Vertices)},
			{"valid", Current->Valid()}
		};
	};
	if (!Editing)
	{
		return DescribePortal(Portal);
	}
	if (Action == "invert")
	{
		AiSceneSnapshot Before;
		Scene->BeforeObjectChange(Portal);
		Portal->InvertOrientation(false);
		Json Result = DescribePortal(Portal);
		FinishChange(Before, Result);
		return Result;
	}
	if (!Creating && Action != "set_geometry")
	{
		return Error("UNKNOWN_ACTION", "Use a supported portal action");
	}
	auto* Front = smart_cast<CSector*>(Find(AiString(Params, "front_id")));
	auto* Back = smart_cast<CSector*>(Find(AiString(Params, "back_id")));
	if (Front == nullptr || Back == nullptr || Front == Back)
	{
		return Error("INVALID_PORTAL_SECTORS", "Specify two different sectors");
	}
	FvectorVec Vertices;
	if (!AiWorld::ReadPortalVertices(Params, Vertices))
	{
		return Error("INVALID_PORTAL_VERTICES", "Provide 3 to 6 ordered world vertices forming a planar convex polygon");
	}
	AiSceneSnapshot Before;
	if (Creating)
	{
		Portal = new CPortal(nullptr, Name);
	}
	else
	{
		Scene->BeforeObjectChange(Portal);
	}
	Portal->Vertices() = std::move(Vertices);
	Portal->SetSectors(Front, Back);
	Portal->SetValid(false);
	if (!Portal->Update(true) || !Portal->Valid())
	{
		if (Creating)
		{
			xr_delete(Portal);
		}
		else
		{
			Before.Restore();
			Synchronize();
		}
		return Error("PORTAL_BUILD_FAILED", "The SDK rejected the portal geometry");
	}
	if (Creating)
	{
		Scene->AppendObject(Portal, false);
	}
	GWorldPartition.MarkDirty();
	Json Result = DescribePortal(Portal);
	FinishChange(Before, Result);
	return Result;
}
