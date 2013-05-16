/**
 * This file was only for testing purposes and will
 * be deleted soon.
 */

#ifndef AI_FLIX2_RECRUITMENT_HPP_INCLUDED
#define AI_FLIX2_RECRUITMENT_HPP_INCLUDED

#include "../composite/rca.hpp"
#include "../../team.hpp"

#ifdef _MSC_VER
#pragma warning(push)
//silence "inherits via dominance" warnings
#pragma warning(disable:4250)
#endif

namespace ai {

namespace flix2_recruitment {

typedef std::map<std::string, std::map<std::string, double> > effectivness_table;

class recruitment : public candidate_action {
public:
	recruitment( rca_context &context , const config &cfg );
	virtual ~recruitment();
	virtual double evaluate();
	virtual void execute();
private:
	/**
	 * The following functions are copied from ca.*
	 * In general it would be nicer to declare those
	 * functions as public static in ca.* (or even somewhere else)
	 * But I did't want to alter ca.* yet.
	 */

	/**
	 * Rates two unit types for their suitability against each other.
	 * Returns 0 if the units are equally matched,
	 * a positive number if a is suited against b,
	 * and a negative number if b is suited against a.
	 */
	int compare_unit_types(const unit_type& a, const unit_type& b) const;

	/**
	 * calculates the average resistance unit type a has against the attacks of
	 * unit type b.
	 */
	int average_resistance_against(const unit_type& a, const unit_type& b) const;

	void print_table(effectivness_table& table) const;

	//This algorithm is based on
	//http://www.rand.org/content/dam/rand/pubs/commercial_books/2007/RAND_CB113-1.pdf
	//Site 219.
	std::map<std::string, double>* findEquilibrium(effectivness_table& table) const;
};

} // of namespace testing_ai_default

} // of namespace ai

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif

