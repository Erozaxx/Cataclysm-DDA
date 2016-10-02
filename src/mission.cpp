#include "mission.h"
#include "game.h"
#include "debug.h"
#include "overmapbuffer.h"
#include "translations.h"
#include "requirements.h"
#include "overmap.h"
#include "line.h"
#include "npc.h"
#include "npc_class.h"

#include <sstream>

#define dbg(x) DebugLog((DebugLevel)(x),D_GAME) << __FILE__ << ":" << __LINE__ << ": "

mission mission_type::create( const int npc_id ) const
{
    mission ret;
    ret.uid = g->assign_mission_id();
    ret.type = this;
    ret.npc_id = npc_id;
    ret.item_id = item_id;
    ret.item_count = item_count;
    ret.value = value;
    ret.follow_up = follow_up;

    if (deadline_low != 0 || deadline_high != 0) {
        ret.deadline = int(calendar::turn) + rng(deadline_low, deadline_high);
    } else {
        ret.deadline = 0;
    }

    return ret;
}

std::unordered_map<int, mission> mission::active_missions;

mission* mission::reserve_new( const mission_type_id type, const int npc_id )
{
    const mission tmp = mission_type::get( type )->create( npc_id );
    auto &result = active_missions[tmp.uid];
    result = tmp;
    return &result;
}

mission *mission::find( int id )
{
    const auto iter = active_missions.find( id );
    if( iter != active_missions.end() ) {
        return &iter->second;
    }
    dbg( D_ERROR ) << "requested mission with uid " << id << " does not exist";
    debugmsg( "requested mission with uid %d does not exist", id );
    return nullptr;
}

void mission::process_all()
{
    for( auto & e : active_missions ) {
        auto &m = e.second;
        if( m.deadline > 0 && !m.failed && int( calendar::turn ) > m.deadline ) {
            m.fail();
        }
    }
}

std::vector<mission*> mission::to_ptr_vector( const std::vector<int> &vec )
{
    std::vector<mission*> result;
    for( auto &id : vec ) {
        const auto miss = find( id );
        if( miss != nullptr ) {
            result.push_back( miss );
        }
    }
    return result;
}

std::vector<int> mission::to_uid_vector( const std::vector<mission*> &vec )
{
    std::vector<int> result;
    for( auto &miss : vec ) {
        result.push_back( miss->uid );
    }
    return result;
}

void mission::clear_all()
{
    active_missions.clear();
}

void mission::on_creature_death( Creature &poor_dead_dude )
{
    if( poor_dead_dude.is_hallucination() ) {
        return;
    }
    monster *mon = dynamic_cast<monster *>( &poor_dead_dude );
    if( mon != nullptr ) {
        if( mon->mission_id == -1 ) {
            return;
        }
        const auto mission = mission::find( mon->mission_id );
        const auto type = mission->type;
        if( type->goal == MGOAL_FIND_MONSTER ) {
            mission->fail();
        }
        if( type->goal == MGOAL_KILL_MONSTER ) {
            mission->step_complete( 1 );
        }
        return;
    }
    npc *p = dynamic_cast<npc *>( &poor_dead_dude );
    if( p == nullptr ) {
        // Must be the player
        for( auto &miss : g->u.get_active_missions() ) {
            // mission is free and can be reused
            miss->player_id = -1;
        }
        // The missions remains assigned to the (dead) character. This should not cause any problems
        // as the character is dismissed anyway.
        // Technically, the active missions could be moved to the failed mission section.
        return;
    }
    const auto dead_guys_id = p->getID();
    for( auto & e : active_missions ) {
        auto &i = e.second;
        //complete the mission if you needed killing
        if( i.type->goal == MGOAL_ASSASSINATE && i.target_npc_id == dead_guys_id ) {
            i.step_complete( 1 );
        }
        //fail the mission if the mission giver dies
        if( i.npc_id == dead_guys_id ) {
            i.fail();
        }
        //fail the mission if recruit target dies
        if( i.type->goal == MGOAL_RECRUIT_NPC && i.target_npc_id == dead_guys_id ) {
            i.fail();
        }
    }
}

mission* mission::reserve_random( const mission_origin origin, const tripoint &p, const int npc_id )
{
    const auto type = mission_type::get_random_id( origin, p );
    if( type.is_null() ) {
        return nullptr;
    }
    return mission::reserve_new( type, npc_id );
}

void mission::assign( player &u )
{
    if( player_id == u.getID() ) {
        debugmsg( "strange: player is already assigned to mission %d", uid );
        return;
    }
    if( player_id != -1 ) {
        debugmsg( "tried to assign mission %d to player, but mission is already assigned to %d", uid, player_id );
        return;
    }
    player_id = u.getID();
    u.on_mission_assignment( *this );
    if( !was_started ) {
        type->start( this );
        was_started = true;
    }
}

void mission::fail()
{
    failed = true;
    if( g->u.getID() == player_id ) {
        g->u.on_mission_finished( *this );
    }

    type->fail( this );
}

void mission::set_target_to_mission_giver()
{
    const auto giver = g->find_npc( npc_id );
    if( giver != nullptr ) {
        target = giver->global_omt_location();
    } else {
        target = overmap::invalid_tripoint;
    }
}

void mission::step_complete( const int _step )
{
    step = _step;
    switch( type->goal ) {
        case MGOAL_FIND_ITEM:
        case MGOAL_FIND_MONSTER:
        case MGOAL_ASSASSINATE:
        case MGOAL_KILL_MONSTER:
            // Go back and report.
            set_target_to_mission_giver();
            break;
        default:
            //Suppress warnings
            break;
    }
}

void mission::wrap_up()
{
    auto &u = g->u;
    if( u.getID() != player_id ) {
        // This is called from npctalk.cpp, the npc should only offer the option to wrap up mission
        // that have been assigned to the current player.
        debugmsg( "mission::wrap_up called, player %d was assigned, but current player is %d", player_id, u.getID() );
    }
    u.on_mission_finished( *this );
    std::vector<item_comp> comps;
    switch( type->goal ) {
        case MGOAL_FIND_ITEM:
            comps.push_back(item_comp(type->item_id, item_count));
            u.consume_items(comps);
            break;
        case MGOAL_FIND_ANY_ITEM:
            u.remove_mission_items( uid );
            break;
        default:
            //Suppress warnings
            break;
    }

    type->end( this );
}

bool mission::is_complete( const int _npc_id ) const
{
    // TODO: maybe not g->u, but more generalized?
    auto &u = g->u;
    inventory tmp_inv = u.crafting_inventory();
    switch( type->goal ) {
        case MGOAL_GO_TO:
            {
                const tripoint cur_pos = g->u.global_omt_location();
                return ( rl_dist( cur_pos, target ) <= 1 );
            }
            break;

        case MGOAL_GO_TO_TYPE:
            {
                const auto cur_ter = overmap_buffer.ter( g->u.global_omt_location() );
                return cur_ter == type->target_id;
            }
            break;

        case MGOAL_FIND_ITEM:
            // TODO: check for count_by_charges and use appropriate player::has_* function
            if (!tmp_inv.has_amount(type->item_id, item_count)) {
                return tmp_inv.has_amount( type->item_id, 1 ) && tmp_inv.has_charges( type->item_id, item_count );
            }
            if( npc_id != -1 && npc_id != _npc_id ) {
                return false;
            }
            return true;

        case MGOAL_FIND_ANY_ITEM:
            return u.has_mission_item( uid ) && ( npc_id == -1 || npc_id == _npc_id );

        case MGOAL_FIND_MONSTER:
            if( npc_id != -1 && npc_id != _npc_id ) {
                return false;
            }
            for( size_t i = 0; i < g->num_zombies(); i++ ) {
                if( g->zombie( i ).mission_id == uid ) {
                    return true;
                }
            }
            return false;

        case MGOAL_RECRUIT_NPC:
            {
                npc *p = g->find_npc( target_npc_id );
                return p != nullptr && p->attitude == NPCATT_FOLLOW;
            }

        case MGOAL_RECRUIT_NPC_CLASS:
            {
                const auto npcs = overmap_buffer.get_npcs_near_player( 100 );
                for( auto & npc : npcs ) {
                    if( npc->myclass == recruit_class && npc->attitude == NPCATT_FOLLOW ) {
                        return true;
                    }
                }
                return false;
            }

        case MGOAL_FIND_NPC:
            return npc_id == _npc_id;

        case MGOAL_ASSASSINATE:
            return step >= 1;

        case MGOAL_KILL_MONSTER:
            return step >= 1;

        case MGOAL_KILL_MONSTER_TYPE:
            debugmsg( "%d kill count", g->kill_count( mtype_id( monster_type ) ) );
            debugmsg( "%d goal", monster_kill_goal );
            return g->kill_count( mtype_id( monster_type ) ) >= monster_kill_goal;

        case MGOAL_COMPUTER_TOGGLE:
            return step >= 1;

        default:
            return false;
    }
    return false;
}

bool mission::has_deadline() const
{
    return deadline != 0;
}

calendar mission::get_deadline() const
{
    return deadline;
}

std::string mission::get_description() const
{
    return description;
}

bool mission::has_target() const
{
    return target != overmap::invalid_tripoint;
}

const tripoint &mission::get_target() const
{
    return target;
}

const mission_type &mission::get_type() const
{
    if( type == nullptr ) {
        debugmsg( "Null mission type" );
        return mission_type::get_all().front();
    }

    return *type;
}

bool mission::has_follow_up() const
{
    return !follow_up.is_null();
}

mission_type_id mission::get_follow_up() const
{
    return follow_up;
}

long mission::get_value() const
{
    return value;
}

int mission::get_id() const
{
    return uid;
}

const std::string &mission::get_item_id() const
{
    return item_id;
}

bool mission::has_failed() const
{
    return failed;
}

int mission::get_npc_id() const
{
    return npc_id;
}

void mission::set_target( const tripoint &new_target )
{
    target = new_target;
}

bool mission::is_assigned() const
{
    return player_id != -1;
}

int mission::get_assigned_player_id() const
{
    return player_id;
}

std::string mission::name()
{
    if (type == NULL) {
        return "NULL";
    }
    return type->name;
}

void mission::load_info(std::istream &data)
{
    int type_id, rewtype, reward_id, rew_skill, tmpfollow, item_num, target_npc_id;
    std::string rew_item, itemid;
    data >> type_id;
    type = mission_type::get( mission_type::from_legacy( type_id ) );
    std::string tmpdesc;
    do {
        data >> tmpdesc;
        if (tmpdesc != "<>") {
            description += tmpdesc + " ";
        }
    } while (tmpdesc != "<>");
    description = description.substr( 0, description.size() - 1 ); // Ending ' '
    data >> failed >> value >> rewtype >> reward_id >> rew_item >> rew_skill >>
         uid >> target.x >> target.y >> itemid >> item_num >> deadline >> npc_id >>
         good_fac_id >> bad_fac_id >> step >> tmpfollow >> target_npc_id;
    target.z = 0;
    follow_up = mission_type::from_legacy(tmpfollow);
    reward.type = npc_favor_type(reward_id);
    reward.item_id = itype_id( rew_item );
    reward.skill = Skill::from_legacy_int( rew_skill );
    item_id = itype_id(itemid);
    item_count = int(item_num);
}

std::string mission::dialogue_for_topic( const std::string &in_topic ) const
{
    // The internal keys are pretty ugly, it's better to translate them here than globally
    static const std::map<std::string, std::string> topic_translation = {{
        { "TALK_MISSION_DESCRIBE", "describe" },
        { "TALK_MISSION_OFFER", "offer" },
        { "TALK_MISSION_ACCEPTED", "accepted" },
        { "TALK_MISSION_REJECTED", "rejected" },
        { "TALK_MISSION_ADVICE", "advice" },
        { "TALK_MISSION_INQUIRE", "inquire" },
        { "TALK_MISSION_SUCCESS", "success" },
        { "TALK_MISSION_SUCCESS_LIE", "success_lie" },
        { "TALK_MISSION_FAILURE", "failure" }
    }};

    const auto &replacement = topic_translation.find( in_topic );
    const std::string &topic = replacement != topic_translation.end() ? replacement->second : in_topic;

    const auto &response = type->dialogue.find( topic );
    if( response != type->dialogue.end() ) {
        return response->second;
    }

    return string_format( "Someone forgot to code this message id is %s, topic is %s!", type->id.c_str(), topic.c_str() );
}

mission::mission()
{
    type = NULL;
    description = "";
    failed = false;
    value = 0;
    uid = -1;
    target = tripoint(INT_MIN, INT_MIN, INT_MIN);
    item_id = "null";
    item_count = 1;
    target_id = oter_id( 0 );
    recruit_class = NC_NONE;
    target_npc_id = -1;
    monster_type = "mon_null";
    monster_kill_goal = -1;
    deadline = 0;
    npc_id = -1;
    good_fac_id = -1;
    bad_fac_id = -1;
    step = 0;
    player_id = -1;
    was_started = false;
}

mission_type::mission_type(mission_type_id ID, std::string NAME, mission_goal GOAL, int DIF, int VAL,
                 bool URGENT,
                 std::function<bool(const tripoint &)> PLACE,
                 std::function<void(mission *)> START,
                 std::function<void(mission *)> END,
                 std::function<void(mission *)> FAIL) :
        id (ID), name (NAME), goal (GOAL), difficulty (DIF), value (VAL),
        urgent(URGENT), place (PLACE), start (START), end (END), fail (FAIL)
{
};

