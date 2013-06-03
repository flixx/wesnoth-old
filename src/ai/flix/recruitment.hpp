/*
   Copyright (C) 2013 by Felix Bauer
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * Experimental Recruitment Engine by Flix
 * Uses Game Theory to find the best mix to recruit
 * when only the enemies recruitment-list is available.
 *
 * See http://wiki.wesnoth.org/User:Flixx/Game_Theory_for_Recruiting
 * for a full explanation
 * @file
 */

#ifndef AI_FLIX_RECRUITMENT_HPP_INCLUDED
#define AI_FLIX_RECRUITMENT_HPP_INCLUDED

#ifdef _MSC_VER
#pragma warning(push)
//silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix_recruitment {

typedef std::map<std::string, double> score_map;

class recruitment : public candidate_action {
public:
	recruitment( rca_context &context , const config &cfg );
	virtual ~recruitment();
	virtual double evaluate();
	virtual void execute();
};

} // of namespace testing_ai_default

} // of namespace ai

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
