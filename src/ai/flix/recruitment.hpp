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

typedef std::map<std::string, double> table_row;
typedef std::map<std::string, table_row> effectivness_table;

//this is a pair of <columnname, effectiveness>
typedef table_row::value_type table_cell;

//this is a pair of <rowname, table_row>
typedef effectivness_table::value_type table_row_pair;


class recruitment : public candidate_action {
public:
	recruitment( rca_context &context , const config &cfg );
	virtual ~recruitment();
	virtual double evaluate();
	virtual void execute();
private:
	/**
	 * This is a modified version of
	 * average_resistance_against in ai/testing/ca.*pp
	 *
	 * calculates the average resistance unit type a
	 * has against the attacks of unit type b.
	 */
	int average_resistance_against(const unit_type& a, const unit_type& b) const;

	void print_table(effectivness_table& table) const;

	//This algorithm is based on
	//http://www.rand.org/content/dam/rand/pubs/commercial_books/2007/RAND_CB113-1.pdf
	//Site 219.
	const std::map<std::string, double> findEquilibrium(effectivness_table& table) const;
};

} // of namespace testing_ai_default

} // of namespace ai

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif

