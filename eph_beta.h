/*
 * Authors of the extension Artur Tamm, Alfredo Correa
 * e-mail: artur.tamm.work@gmail.com
 */

#ifndef EPH_BETA
#define EPH_BETA

// external headers 
#include <vector>
#include <string>
#include <cassert>

// internal headers
#include "eph_spline.h"

#if 1
template<typename Container = std::vector<double>>
class EPH_Beta {
  public:
    EPH_Beta() {}
    EPH_Beta(const char* file) {
      
    }
  
  private:
    static constexpr unsigned int max_line_length = 1024; // this is for parsing
    double r_cutoff; // cutoff for locality
    double rho_cutoff; // cutoff for largest site density
    size_t n_elements; // number of elements
    
    std::vector<uint8_t> element_number;
    std::vector<std::string> element_name;
    std::vector<EPH_Spline<Container>> rho;
    std::vector<EPH_Spline<Container>> beta;
};
#endif

#if 0
template<typename Float = double, template<typename> class Allocator = std::allocator, template <typename _F = Float, typename _A = Allocator<Float>> class Container = std::vector>
class EPH_Beta {
  public:
    EPH_Beta() {}
    EPH_Beta(const char* file) {
      
    }
  
  private:
    static constexpr unsigned int max_line_length = 1024; // this is for parsing
    double r_cutoff; // cutoff for locality
    double rho_cutoff; // cutoff for largest site density
    size_t n_elements; // number of elements
    
    std::vector<int> elementNumber;
    std::vector<std::string> elementName;
    std::vector<EPH_Spline> rho;
    std::vector<EPH_Spline> beta;
};
#endif


#if 0
#include <stdexcept>

class EPH_Beta {
  public:
    EPH_Beta(const char* file); // initialise EPH_Beta based on file
    EPH_Beta() = delete;
    
    // how many types are described by this class
    inline int getElementsNumber() const {
      return elementName.size();
    }
    
    // get element name described by the class
    inline std::string getName(const unsigned int element) const {
      #ifndef DNDEBUG
      if(element >= elementName.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return elementName[element];
    }
    
    // get a cutoff used in rho(r)
    inline double getCutoff() const {
      return rcut;
    }
    
    // get a cutoff used in beta(rho)
    inline double getRhoCutoff() const {
      return rhocut;
    }
    
    // get the periodic table number for an element (this is not used)
    inline unsigned int getNumber(const unsigned int element) const {
      #ifndef DNDEBUG
      if(element >= elementNumber.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return elementNumber[element];
    }
    
    // return the density value
    inline double getRho(const unsigned int element, const double r) const {
      #ifndef DNDEBUG
      if(element >= rho.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return rho[element].GetValue(r);
    }
    
    // return the derivative of the density
    inline double getDRho(const unsigned int element, const double r) const {
      #ifndef DNDEBUG
      if(element >= rho.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return rho[element].GetDValue(r);
    }
    
    // get coupling parameter value
    inline double getBeta(const unsigned int element, const double rho) const {
      #ifndef EPH_UNSAFE
      if(element > beta.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return beta[element].GetValue(rho);
    }
    
    // get the derivative of the coupling parameter
    inline double getDBeta(const unsigned int element, const double rho) const {
      #ifndef DNDEBUG
      if(element > beta.size()) throw std::runtime_error("eph_beta: out of scope index provided");
      #endif
      return beta[element].GetDValue(rho);
    }
  
  private:
    static constexpr unsigned int lineLength = 1024; // this is for parsing
    double rcut;
    double rhocut;
    
    std::vector<int> elementNumber;
    std::vector<std::string> elementName;
    std::vector<EPH_Spline> rho;
    std::vector<EPH_Spline> beta;
};
#endif

#endif
