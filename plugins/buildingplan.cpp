#include "df/construction_type.h"
#include "df/entity_position.h"
#include "df/interface_key.h"
#include "df/ui_build_selector.h"
#include "df/viewscreen_dwarfmodest.h"

#include "modules/Gui.h"
#include "modules/Maps.h"
#include "modules/World.h"

#include "LuaTools.h"
#include "PluginManager.h"

#include "uicommon.h"
#include "listcolumn.h"
#include "buildingplan-lib.h"

DFHACK_PLUGIN("buildingplan");
#define PLUGIN_VERSION 2.0
REQUIRE_GLOBAL(ui);
REQUIRE_GLOBAL(ui_build_selector);
REQUIRE_GLOBAL(world);

#define MAX_MASK 10
#define MAX_MATERIAL 21

bool show_help = false;
bool quickfort_mode = false;
bool in_dummy_screen = false;
std::unordered_map<BuildingTypeKey, bool, BuildingTypeKeyHash> planmode_enabled;

class ViewscreenChooseMaterial : public dfhack_viewscreen
{
public:
    ViewscreenChooseMaterial(ItemFilter &filter);

    void feed(set<df::interface_key> *input);

    void render();

    std::string getFocusString() { return "buildingplan_choosemat"; }

private:
    ListColumn<df::dfhack_material_category> masks_column;
    ListColumn<MaterialInfo> materials_column;
    int selected_column;
    ItemFilter &filter;

    void addMaskEntry(df::dfhack_material_category &mask, const std::string &text)
    {
        auto entry = ListEntry<df::dfhack_material_category>(pad_string(text, MAX_MASK, false), mask);
        if (filter.matches(mask))
            entry.selected = true;

        masks_column.add(entry);
    }

    void populateMasks()
    {
        masks_column.clear();
        df::dfhack_material_category mask;

        mask.whole = 0;
        mask.bits.stone = true;
        addMaskEntry(mask, "Stone");

        mask.whole = 0;
        mask.bits.wood = true;
        addMaskEntry(mask, "Wood");

        mask.whole = 0;
        mask.bits.metal = true;
        addMaskEntry(mask, "Metal");

        mask.whole = 0;
        mask.bits.soap = true;
        addMaskEntry(mask, "Soap");

        masks_column.filterDisplay();
    }

    void populateMaterials()
    {
        materials_column.clear();
        df::dfhack_material_category selected_category;
        std::vector<df::dfhack_material_category> selected_masks = masks_column.getSelectedElems();
        if (selected_masks.size() == 1)
            selected_category = selected_masks[0];
        else if (selected_masks.size() > 1)
            return;

        df::world_raws &raws = world->raws;
        for (int i = 1; i < DFHack::MaterialInfo::NUM_BUILTIN; i++)
        {
            auto obj = raws.mat_table.builtin[i];
            if (obj)
            {
                MaterialInfo material;
                material.decode(i, -1);
                addMaterialEntry(selected_category, material, material.toString());
            }
        }

        for (size_t i = 0; i < raws.inorganics.size(); i++)
        {
            df::inorganic_raw *p = raws.inorganics[i];
            MaterialInfo material;
            material.decode(0, i);
            addMaterialEntry(selected_category, material, material.toString());
        }

        decltype(selected_category) wood_flag;
        wood_flag.bits.wood = true;
        if (!selected_category.whole || selected_category.bits.wood)
        {
            for (size_t i = 0; i < raws.plants.all.size(); i++)
            {
                df::plant_raw *p = raws.plants.all[i];
                for (size_t j = 0; p->material.size() > 1 && j < p->material.size(); j++)
                {
                    auto t = p->material[j];
                    if (p->material[j]->id != "WOOD")
                        continue;

                    MaterialInfo material;
                    material.decode(DFHack::MaterialInfo::PLANT_BASE+j, i);
                    auto name = material.toString();
                    ListEntry<MaterialInfo> entry(pad_string(name, MAX_MATERIAL, false), material);
                    if (filter.matches(material))
                        entry.selected = true;

                    materials_column.add(entry);
                }
            }
        }
        materials_column.sort();
    }

    void addMaterialEntry(df::dfhack_material_category &selected_category,
                          MaterialInfo &material, std::string name)
    {
        if (!selected_category.whole || material.matches(selected_category))
        {
            ListEntry<MaterialInfo> entry(pad_string(name, MAX_MATERIAL, false), material);
            if (filter.matches(material))
                entry.selected = true;

            materials_column.add(entry);
        }
    }

    void validateColumn()
    {
        set_to_limit(selected_column, 1);
    }

    void resize(int32_t x, int32_t y)
    {
        dfhack_viewscreen::resize(x, y);
        masks_column.resize();
        materials_column.resize();
    }
};

const DFHack::MaterialInfo &material_info_identity_fn(const DFHack::MaterialInfo &m) { return m; }

ViewscreenChooseMaterial::ViewscreenChooseMaterial(ItemFilter &filter)
    : filter(filter)
{
    selected_column = 0;
    masks_column.setTitle("Type");
    masks_column.multiselect = true;
    masks_column.allow_search = false;
    masks_column.left_margin = 2;
    materials_column.left_margin = MAX_MASK + 3;
    materials_column.setTitle("Material");
    materials_column.multiselect = true;

    masks_column.changeHighlight(0);

    populateMasks();
    populateMaterials();

    masks_column.selectDefaultEntry();
    materials_column.selectDefaultEntry();
    materials_column.changeHighlight(0);
}

void ViewscreenChooseMaterial::feed(set<df::interface_key> *input)
{
    bool key_processed = false;
    switch (selected_column)
    {
    case 0:
        key_processed = masks_column.feed(input);
        if (input->count(interface_key::SELECT))
            populateMaterials(); // Redo materials lists based on category selection
        break;
    case 1:
        key_processed = materials_column.feed(input);
        break;
    }

    if (key_processed)
        return;

    if (input->count(interface_key::LEAVESCREEN))
    {
        input->clear();
        Screen::dismiss(this);
        return;
    }
    if (input->count(interface_key::CUSTOM_SHIFT_C))
    {
        filter.clear();
        masks_column.clearSelection();
        materials_column.clearSelection();
        populateMaterials();
    }
    else if (input->count(interface_key::SEC_SELECT))
    {
        // Convert list selections to material filters
        filter.clearMaterialMask();

        // Category masks
        auto masks = masks_column.getSelectedElems();
        for (auto it = masks.begin(); it != masks.end(); ++it)
            filter.addMaterialMask(it->whole);

        // Specific materials
        auto materials = materials_column.getSelectedElems();
        std::vector<DFHack::MaterialInfo> materialInfos;
        transform_(materials, materialInfos, material_info_identity_fn);
        filter.setMaterials(materialInfos);

        Screen::dismiss(this);
    }
    else if (input->count(interface_key::CURSOR_LEFT))
    {
        --selected_column;
        validateColumn();
    }
    else if (input->count(interface_key::CURSOR_RIGHT))
    {
        selected_column++;
        validateColumn();
    }
    else if (enabler->tracking_on && enabler->mouse_lbut)
    {
        if (masks_column.setHighlightByMouse())
            selected_column = 0;
        else if (materials_column.setHighlightByMouse())
            selected_column = 1;

        enabler->mouse_lbut = enabler->mouse_rbut = 0;
    }
}

void ViewscreenChooseMaterial::render()
{
    if (Screen::isDismissed(this))
        return;

    dfhack_viewscreen::render();

    Screen::clear();
    Screen::drawBorder("  Building Material  ");

    masks_column.display(selected_column == 0);
    materials_column.display(selected_column == 1);

    int32_t y = gps->dimy - 3;
    int32_t x = 2;
    OutputHotkeyString(x, y, "Toggle", interface_key::SELECT);
    x += 3;
    OutputHotkeyString(x, y, "Save", interface_key::SEC_SELECT);
    x += 3;
    OutputHotkeyString(x, y, "Clear", interface_key::CUSTOM_SHIFT_C);
    x += 3;
    OutputHotkeyString(x, y, "Cancel", interface_key::LEAVESCREEN);
}

//START Viewscreen Hook
static bool is_planmode_enabled(BuildingTypeKey key)
{
    return planmode_enabled[key] || quickfort_mode;
}

static std::string get_item_label(const BuildingTypeKey &key, int item_idx)
{
     auto L = Lua::Core::State;
     color_ostream_proxy out(Core::getInstance().getConsole());
     Lua::StackUnwinder top(L);

    if (!lua_checkstack(L, 5) ||
        !Lua::PushModulePublic(
            out, L, "plugins.buildingplan", "get_item_label"))
        return "Failed push";

    Lua::Push(L, std::get<0>(key));
    Lua::Push(L, std::get<1>(key));
    Lua::Push(L, std::get<2>(key));
    Lua::Push(L, item_idx);

    if (!Lua::SafeCall(out, L, 4, 1))
        return "Failed call";

    const char *s = lua_tostring(L, -1);
    if (!s)
        return "No string";

    lua_pop(L, 1);
    return s;
}

static bool construct_planned_building()
{
     auto L = Lua::Core::State;
     color_ostream_proxy out(Core::getInstance().getConsole());

     CoreSuspendClaimer suspend;
     Lua::StackUnwinder top(L);

    if (!(lua_checkstack(L, 1) &&
          Lua::PushModulePublic(out, L, "plugins.buildingplan",
                                "construct_building_from_ui_state") &&
          Lua::SafeCall(out, L, 0, 1)))
    {
        return false;
    }

    auto bld = static_cast<df::building *>(LuaWrapper::get_object_ref(L, -1));
    lua_pop(L, 1);

    if (!bld)
        return false;

    planner.addPlannedBuilding(bld);

    return true;
}

struct buildingplan_query_hook : public df::viewscreen_dwarfmodest
{
    typedef df::viewscreen_dwarfmodest interpose_base;

    // no non-static fields allowed (according to VTableInterpose.h)
    static df::building *bld;
    static PlannedBuilding *pb;
    static int filter_count;
    static int filter_idx;

    // logic is reversed since we're starting at the last filter
    bool hasNextFilter() const { return filter_idx > 0; }
    bool hasPrevFilter() const { return filter_idx + 1 < filter_count; }

    bool isInPlannedBuildingQueryMode()
    {
        return (ui->main.mode == df::ui_sidebar_mode::QueryBuilding ||
            ui->main.mode == df::ui_sidebar_mode::BuildingItems) &&
            planner.getPlannedBuilding(world->selected_building);
    }

    // reinit static fields when selected building changes
    void initStatics()
    {
        df::building *cur_bld = world->selected_building;
        if (bld != cur_bld)
        {
            bld = cur_bld;
            pb = planner.getPlannedBuilding(bld);
            filter_count = pb->getFilters().size();
            filter_idx = filter_count - 1;
        }
    }

    bool handleInput(set<df::interface_key> *input)
    {
        if (!isInPlannedBuildingQueryMode())
            return false;

        initStatics();

        if (input->count(interface_key::SUSPENDBUILDING))
            return true; // Don't unsuspend planned buildings
        if (input->count(interface_key::DESTROYBUILDING))
        {
            // remove persistent data
            pb->remove();
            // still allow the building to be removed
            return false;
        }

        // ctrl+Right
        if (input->count(interface_key::A_MOVE_E_DOWN) && hasNextFilter())
            --filter_idx;
        // ctrl+Left
        else if (input->count(interface_key::A_MOVE_W_DOWN) && hasPrevFilter())
            ++filter_idx;
        else
            return false;
        return true;
    }

    DEFINE_VMETHOD_INTERPOSE(void, feed, (set<df::interface_key> *input))
    {
        if (!handleInput(input))
            INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        INTERPOSE_NEXT(render)();

        if (!isInPlannedBuildingQueryMode())
            return;

        initStatics();

        // Hide suspend toggle option
        auto dims = Gui::getDwarfmodeViewDims();
        int left_margin = dims.menu_x1 + 1;
        int x = left_margin;
        int y = 20;
        Screen::Pen pen(' ', COLOR_BLACK);
        Screen::fillRect(pen, x, y, dims.menu_x2, y);

        auto & filter = pb->getFilters()[filter_idx];
        y = 24;
        std::string item_label =
            stl_sprintf("Item %d of %d", filter_count - filter_idx, filter_count);
        OutputString(COLOR_WHITE, x, y, "Planned Building Filter", true, left_margin + 1);
        OutputString(COLOR_WHITE, x, y, item_label.c_str(), true, left_margin + 1);
        OutputString(COLOR_WHITE, x, y, get_item_label(toBuildingTypeKey(bld), filter_idx).c_str(), true, left_margin);
        ++y;
        OutputString(COLOR_BROWN, x, y, "Min Quality: ", false, left_margin);
        OutputString(COLOR_BLUE, x, y, filter.getMinQuality(), true, left_margin);
        OutputString(COLOR_BROWN, x, y, "Max Quality: ", false, left_margin);
        OutputString(COLOR_BLUE, x, y, filter.getMaxQuality(), true, left_margin);

        if (filter.getDecoratedOnly())
            OutputString(COLOR_BLUE, x, y, "Decorated Only", true, left_margin);

        OutputString(COLOR_BROWN, x, y, "Materials:", true, left_margin);
        auto filters = filter.getMaterials();
        for (auto it = filters.begin(); it != filters.end(); ++it)
            OutputString(COLOR_BLUE, x, y, "*" + *it, true, left_margin);

        ++y;
        if (hasPrevFilter())
            OutputHotkeyString(x, y, "Prev Item", "Ctrl+Left", true, left_margin);
        if (hasNextFilter())
            OutputHotkeyString(x, y, "Next Item", "Ctrl+Right", true, left_margin);
    }
};

df::building * buildingplan_query_hook::bld;
PlannedBuilding * buildingplan_query_hook::pb;
int buildingplan_query_hook::filter_count;
int buildingplan_query_hook::filter_idx;

struct buildingplan_place_hook : public df::viewscreen_dwarfmodest
{
    typedef df::viewscreen_dwarfmodest interpose_base;

    // no non-static fields allowed (according to VTableInterpose.h)
    static BuildingTypeKey key;
    static std::vector<ItemFilter>::reverse_iterator filter_rbegin;
    static std::vector<ItemFilter>::reverse_iterator filter_rend;
    static std::vector<ItemFilter>::reverse_iterator filter;
    static int filter_count;
    static int filter_idx;

    bool hasNextFilter() const { return filter + 1 != filter_rend; }
    bool hasPrevFilter() const { return filter != filter_rbegin; }

    bool isInPlannedBuildingPlacementMode()
    {
        return ui->main.mode == ui_sidebar_mode::Build &&
            df::global::ui_build_selector &&
            df::global::ui_build_selector->stage < 2 &&
            planner.isPlannableBuilding(toBuildingTypeKey(ui_build_selector));
    }

    // reinit static fields when selected building type changes
    void initStatics()
    {
        BuildingTypeKey cur_key = toBuildingTypeKey(ui_build_selector);
        if (key != cur_key)
        {
            key = cur_key;
            auto wrapper = planner.getItemFilters(key);
            filter_rbegin = wrapper.rbegin();
            filter_rend = wrapper.rend();
            filter = filter_rbegin;
            filter_count = wrapper.get().size();
            filter_idx = filter_count - 1;
        }
    }

    bool handleInput(set<df::interface_key> *input)
    {
        if (!isInPlannedBuildingPlacementMode())
        {
            show_help = false;
            return false;
        }
        
        initStatics();

        if (in_dummy_screen)
        {
            if (input->count(interface_key::SELECT) || input->count(interface_key::SEC_SELECT)
                 || input->count(interface_key::LEAVESCREEN))
            {
                in_dummy_screen = false;
                // pass LEAVESCREEN up to parent view
                input->clear();
                input->insert(interface_key::LEAVESCREEN);
                return false;
            }
            return true;
        }

        if (input->count(interface_key::CUSTOM_P) ||
            input->count(interface_key::CUSTOM_F) ||
            input->count(interface_key::CUSTOM_D) ||
            input->count(interface_key::CUSTOM_Q) ||
            input->count(interface_key::CUSTOM_W) ||
            input->count(interface_key::CUSTOM_A) ||
            input->count(interface_key::CUSTOM_S) ||
            input->count(interface_key::CUSTOM_M))
        {
            show_help = true;
        }

        if (input->count(interface_key::CUSTOM_SHIFT_P))
        {
            planmode_enabled[key] = !planmode_enabled[key];
            if (!is_planmode_enabled(key))
                Gui::refreshSidebar();
            return true;
        }
        if (input->count(interface_key::CUSTOM_SHIFT_F))
        {
            quickfort_mode = !quickfort_mode;
            return true;
        }

        if (!is_planmode_enabled(key))
            return false;

        if (input->count(interface_key::SELECT))
        {
            if (ui_build_selector->errors.size() == 0 && construct_planned_building())
            {
                Gui::refreshSidebar();
                if (quickfort_mode)
                    in_dummy_screen = true;
            }
            return true;
        }

        if (input->count(interface_key::CUSTOM_SHIFT_M))
            Screen::show(dts::make_unique<ViewscreenChooseMaterial>(*filter), plugin_self);
        else if (input->count(interface_key::CUSTOM_SHIFT_Q))
            filter->decMinQuality();
        else if (input->count(interface_key::CUSTOM_SHIFT_W))
            filter->incMinQuality();
        else if (input->count(interface_key::CUSTOM_SHIFT_A))
            filter->decMaxQuality();
        else if (input->count(interface_key::CUSTOM_SHIFT_S))
            filter->incMaxQuality();
        else if (input->count(interface_key::CUSTOM_SHIFT_D))
            filter->toggleDecoratedOnly();
        // ctrl+Right
        else if (input->count(interface_key::A_MOVE_E_DOWN) && hasNextFilter())
        {
            ++filter;
            --filter_idx;
        }
        // ctrl+Left
        else if (input->count(interface_key::A_MOVE_W_DOWN) && hasPrevFilter())
        {
            --filter;
            ++filter_idx;
        }
        else
            return false;
        return true;
    }

    DEFINE_VMETHOD_INTERPOSE(void, feed, (set<df::interface_key> *input))
    {
        if (!handleInput(input))
            INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        bool plannable = isInPlannedBuildingPlacementMode();
        if (plannable && is_planmode_enabled(key))
        {
            if (ui_build_selector->stage < 1)
                // No materials but turn on cursor
                ui_build_selector->stage = 1;

            for (auto iter = ui_build_selector->errors.begin();
                 iter != ui_build_selector->errors.end();)
            {
                // FIXME Hide bags
                if (((*iter)->find("Needs") != string::npos
                     && **iter != "Needs adjacent wall")
                    || (*iter)->find("No access") != string::npos)
                    iter = ui_build_selector->errors.erase(iter);
                else
                    ++iter;
            }
        }

        INTERPOSE_NEXT(render)();

        if (!plannable)
            return;

        initStatics();

        auto dims = Gui::getDwarfmodeViewDims();
        int left_margin = dims.menu_x1 + 1;
        int x = left_margin;

        if (in_dummy_screen)
        {
            Screen::Pen pen(' ',COLOR_BLACK);
            int y = dims.y1 + 1;
            Screen::fillRect(pen, x, y, dims.menu_x2, y + 20);

            ++y;

            OutputString(COLOR_BROWN, x, y,
                "Placeholder for legacy Quickfort. This screen is not required for DFHack native quickfort.",
                true, left_margin);
            OutputString(COLOR_WHITE, x, y, "Enter, Shift-Enter or Esc", true, left_margin);
            return;
        }

        int y = 23;

        if (ui_build_selector->building_type == df::building_type::Construction
            && ui_build_selector->building_subtype <
               df::construction_type::TrackN)
        {
            // try not to conflict with the automaterial plugin UI
            y = 34;
        }

        if (show_help)
        {
            OutputString(COLOR_BROWN, x, y, "Note: ");
            OutputString(COLOR_WHITE, x, y, "Use Shift-Keys here", true, left_margin);
        }

        OutputToggleString(x, y, "Planning Mode", "P", planmode_enabled[key], true, left_margin);
        OutputToggleString(x, y, "Quickfort Mode", "F", quickfort_mode, true, left_margin);

        if (!is_planmode_enabled(key))
            return;

        y += 2;
        std::string title =
            stl_sprintf("Filter for Item %d of %d:",
                        filter_count - filter_idx, filter_count);
        OutputString(COLOR_WHITE, x, y, title.c_str(), true, left_margin + 1);
        OutputString(COLOR_WHITE, x, y, get_item_label(key, filter_idx).c_str(), true, left_margin);

        OutputHotkeyString(x, y, "Min Quality: ", "QW");
        OutputString(COLOR_BROWN, x, y, filter->getMinQuality(), true, left_margin);

        OutputHotkeyString(x, y, "Max Quality: ", "AS");
        OutputString(COLOR_BROWN, x, y, filter->getMaxQuality(), true, left_margin);

        OutputToggleString(x, y, "Decorated Only", "D", filter->getDecoratedOnly(), true, left_margin);

        OutputHotkeyString(x, y, "Material Filter:", "M", true, left_margin);
        auto filter_descriptions = filter->getMaterials();
        for (auto it = filter_descriptions.begin();
             it != filter_descriptions.end(); ++it)
             OutputString(COLOR_BROWN, x, y, "   *" + *it, true, left_margin);

        y += 2;
        if (hasPrevFilter())
            OutputHotkeyString(x, y, "Prev Item", "Ctrl+Left", true, left_margin);
        if (hasNextFilter())
            OutputHotkeyString(x, y, "Next Item", "Ctrl+Right", true, left_margin);
    }
};

BuildingTypeKey buildingplan_place_hook::key;
std::vector<ItemFilter>::reverse_iterator buildingplan_place_hook::filter_rbegin;
std::vector<ItemFilter>::reverse_iterator buildingplan_place_hook::filter_rend;
std::vector<ItemFilter>::reverse_iterator buildingplan_place_hook::filter;
int buildingplan_place_hook::filter_count;
int buildingplan_place_hook::filter_idx;

struct buildingplan_room_hook : public df::viewscreen_dwarfmodest
{
    typedef df::viewscreen_dwarfmodest interpose_base;

    std::vector<Units::NoblePosition> getNoblePositionOfSelectedBuildingOwner()
    {
        std::vector<Units::NoblePosition> np;
        if (ui->main.mode != df::ui_sidebar_mode::QueryBuilding ||
            !world->selected_building ||
            !world->selected_building->owner)
        {
            return np;
        }

        switch (world->selected_building->getType())
        {
        case building_type::Bed:
        case building_type::Chair:
        case building_type::Table:
            break;
        default:
            return np;
        }

        return getUniqueNoblePositions(world->selected_building->owner);
    }

    bool isInNobleRoomQueryMode()
    {
        if (getNoblePositionOfSelectedBuildingOwner().size() > 0)
            return canReserveRoom(world->selected_building);
        else
            return false;
    }

    bool handleInput(set<df::interface_key> *input)
    {
        if (!isInNobleRoomQueryMode())
            return false;

        if (Gui::inRenameBuilding())
            return false;
        auto np = getNoblePositionOfSelectedBuildingOwner();
        df::interface_key last_token = get_string_key(input);
        if (last_token >= interface_key::STRING_A048
            && last_token <= interface_key::STRING_A058)
        {
            size_t selection = last_token - interface_key::STRING_A048;
            if (np.size() < selection)
                return false;
            roomMonitor.toggleRoomForPosition(world->selected_building->id, np.at(selection-1).position->code);
            return true;
        }

        return false;
    }

    DEFINE_VMETHOD_INTERPOSE(void, feed, (set<df::interface_key> *input))
    {
        if (!handleInput(input))
            INTERPOSE_NEXT(feed)(input);
    }

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        INTERPOSE_NEXT(render)();

        if (!isInNobleRoomQueryMode())
            return;

        auto np = getNoblePositionOfSelectedBuildingOwner();
        auto dims = Gui::getDwarfmodeViewDims();
        int left_margin = dims.menu_x1 + 1;
        int x = left_margin;
        int y = 24;
        OutputString(COLOR_BROWN, x, y, "DFHack", true, left_margin);
        OutputString(COLOR_WHITE, x, y, "Auto-allocate to:", true, left_margin);
        for (size_t i = 0; i < np.size() && i < 9; i++)
        {
            bool enabled =
                roomMonitor.getReservedNobleCode(world->selected_building->id)
                == np[i].position->code;
            OutputToggleString(x, y, np[i].position->name[0].c_str(),
                int_to_string(i+1).c_str(), enabled, true, left_margin);
        }
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_query_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_place_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_room_hook, feed);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_query_hook, render);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_place_hook, render);
IMPLEMENT_VMETHOD_INTERPOSE(buildingplan_room_hook, render);

static command_result buildingplan_cmd(color_ostream &out, vector <string> & parameters)
{
    if (!parameters.empty())
    {
        if (parameters.size() == 1 && toLower(parameters[0])[0] == 'v')
        {
            out << "Building Plan" << endl << "Version: " << PLUGIN_VERSION << endl;
        }
        else if (parameters.size() == 2 && toLower(parameters[0]) == "debug")
        {
            show_debugging = (toLower(parameters[1]) == "on");
            out << "Debugging " << ((show_debugging) ? "enabled" : "disabled") << endl;
        }
    }

    return CR_OK;
}

DFHACK_PLUGIN_IS_ENABLED(is_enabled);

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable)
{
    if (!gps)
        return CR_FAILURE;

    if (enable != is_enabled)
    {
        planner.reset();

        if (!INTERPOSE_HOOK(buildingplan_query_hook, feed).apply(enable) ||
            !INTERPOSE_HOOK(buildingplan_place_hook, feed).apply(enable) ||
            !INTERPOSE_HOOK(buildingplan_room_hook, feed).apply(enable) ||
            !INTERPOSE_HOOK(buildingplan_query_hook, render).apply(enable) ||
            !INTERPOSE_HOOK(buildingplan_place_hook, render).apply(enable) ||
            !INTERPOSE_HOOK(buildingplan_room_hook, render).apply(enable))
            return CR_FAILURE;

        is_enabled = enable;
    }

    return CR_OK;
}

DFhackCExport command_result plugin_init ( color_ostream &out, std::vector <PluginCommand> &commands)
{
    commands.push_back(
        PluginCommand(
        "buildingplan", "Plan building construction before you have materials",
        buildingplan_cmd, false, "Run 'buildingplan debug [on|off]' to toggle debugging, or 'buildingplan version' to query the plugin version."));

    return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
    switch (event) {
    case SC_MAP_LOADED:
        planner.reset();
        roomMonitor.reset(out);
        break;
    default:
        break;
    }

    return CR_OK;
}

static bool cycle_requested = false;

#define DAY_TICKS 1200
DFhackCExport command_result plugin_onupdate(color_ostream &)
{
    if (Maps::IsValid() && !World::ReadPauseState()
        && (cycle_requested || world->frame_counter % (DAY_TICKS/2) == 0))
    {
        planner.doCycle();
        roomMonitor.doCycle();
        cycle_requested = false;
    }

    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &)
{
    return CR_OK;
}

// Lua API section

static bool isPlannableBuilding(df::building_type type,
                                int16_t subtype,
                                int32_t custom) {
    return planner.isPlannableBuilding(
        toBuildingTypeKey(type, subtype, custom));
}

static void addPlannedBuilding(df::building *bld) {
    planner.addPlannedBuilding(bld);
}

static void doCycle() {
    planner.doCycle();
}

static void scheduleCycle() {
    cycle_requested = true;
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(isPlannableBuilding),
    DFHACK_LUA_FUNCTION(addPlannedBuilding),
    DFHACK_LUA_FUNCTION(doCycle),
    DFHACK_LUA_FUNCTION(scheduleCycle),
    DFHACK_LUA_END
};
