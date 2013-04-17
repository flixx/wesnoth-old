/*
 * recruitment.cpp
 *
 *  Created on: Apr 16, 2013
 *      Author: fehlx
 */

#include <boost/foreach.hpp>
#include <map>

#include "../../actions/attack.hpp"
#include "../../attack_prediction.hpp"
#include "../../resources.hpp"
#include "../../log.hpp"
#include "../../map.hpp"
#include "../../team.hpp"
#include "../../unit_display.hpp"
#include "../../unit_map.hpp"
#include "../../unit_types.hpp"
#include "../composite/rca.hpp"
#include "../default/ai.hpp"

#include "recruitment.hpp"

static lg::log_domain log_ai_flix("ai/flix");
#define DBG_AI_FLIX LOG_STREAM(debug, log_ai_flix)
#define LOG_AI_FLIX LOG_STREAM(info, log_ai_flix)
#define WRN_AI_FLIX LOG_STREAM(warn, log_ai_flix)
#define ERR_AI_FLIX LOG_STREAM(err, log_ai_flix)

#ifdef _MSC_VER
#pragma warning(push)
//silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix_recruitment {

	recruitment::recruitment( rca_context &context , const config &cfg ):
		candidate_action(context, cfg){ 	}
	double recruitment::evaluate(){
		std::vector<unit_map::unit_iterator> leaders = resources::units->find_leaders(get_side());

		BOOST_FOREACH(unit_map::unit_iterator &leader, leaders){
			if (leader == resources::units->end()) {
				return BAD_SCORE;
			}

			std::set<map_location> checked_hexes;
			const map_location &keep = leader->get_location();
			checked_hexes.insert(keep);

			if (resources::game_map->is_keep(leader->get_location()) && count_free_hexes_in_castle(leader->get_location(), checked_hexes) != 0) {
				return get_score();
			}
		}

		return BAD_SCORE;
	}
	void recruitment::execute(){
		LOG_AI_FLIX << "Flix recruitment begin! \n";
		std::set<std::string> options = current_team().recruits();

		recruit_result_ptr recruit_result = execute_recruit_action(*options.begin());
		LOG_AI_FLIX << "Recruited " << *options.begin() << std::endl;

	}

	recruitment::~recruitment()
	{
	}

}

}
