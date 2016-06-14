#include<algorithm>

#include "AutoSchedule.h"
#include "RealizationOrder.h"
#include "FindCalls.h"
#include "Simplify.h"
#include "Substitute.h"
#include "Target.h"
#include "Function.h"
#include "Bounds.h"
#include "Var.h"
#include "IRPrinter.h"
#include "Inline.h"
#include "Func.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;
using std::map;
using std::set;
using std::deque;
using std::pair;
using std::make_pair;

// Utility functions
int get_extent(const Interval& i) {
    if ((i.min.as<IntImm>()) && (i.max.as<IntImm>())) {
        const IntImm * bmin = i.min.as<IntImm>();
        const IntImm * bmax = i.max.as<IntImm>();
        // Count only if the overlap makes sense
        if (bmin->value <= bmax->value)
            return (bmax->value - bmin->value + 1);
        else
            return 0;
    }
    /* TODO Check if this is necessary at some point
       else {
       Expr diff = simplify(i.max - i.min);
       if (diff.as<IntImm>())
       return diff.as<IntImm>()->value;
       } */
    return -1;
}

int64_t box_area(const Box& b) {
    int64_t box_area = 1;
    for (size_t i = 0; i < b.size(); i++) {
        // Maybe should check for unsigned integers and floats too
        int64_t extent = get_extent(b[i]);
        if (extent > 0 && box_area > 0)
            box_area = box_area * extent;
        else if (extent == 0) {
            box_area = 0;
            break;
        } else {
            box_area = -1;
        }
    }
    return box_area;
}

class FindAllCalls : public IRVisitor {
 public:
  set<string> calls;
  using IRVisitor::visit;

  void visit(const Call *call) {
      // See if images need to be included
      if (call->call_type == Call::Halide ||
          call->call_type == Call::Image) {
          calls.insert(call->name);
      }
      for (size_t i = 0; (i < call->args.size()); i++)
          call->args[i].accept(this);
  }
};

class FindImageInputs : public IRVisitor {
 public:
  map<string, Type> input_type;
  using IRVisitor::visit;

  void visit(const Call *call) {
      if (call->call_type == Call::Image) {
          input_type[call->name] = call->type;
      }
      for (size_t i = 0; (i < call->args.size()); i++)
          call->args[i].accept(this);
  }
};

void set_schedule_defaults(map<string, Function>& env) {
    // Changing the default to compute root.

    // TODO: This ignores existing schedules specified by the user atm and needs
    // to be addressed when we have decided on a mechanism to inform the auto
    // scheduler not to mess with an user specified schedule.
    for (auto& kv : env) {
        // TODO:
        // The shedule is marked touched when a user modifies the schedule. The idea
        // is to keep the user specified schedule intact as much as possible.
        // However, user specified schedules can have non-local effects and will not
        // be captured by touched.  For example: f.compute_at(g, y) now specifies
        // constraints on the schedule of f as well as the schedule of g i.e., the
        // variable y in g cannot be split or reordered since that may change user
        // intent.
        //
        // Open question is how to deal with the constraints induced by user
        // specified schedules.
        kv.second.schedule().store_level() = LoopLevel::root();
        kv.second.schedule().compute_level() = LoopLevel::root();

        // Initializing the schedules for update definitions
        for (size_t u = 0; u < kv.second.updates().size(); u++) {
            kv.second.update_schedule(u).store_level() = LoopLevel::root();
            kv.second.update_schedule(u).compute_level() = LoopLevel::root();
        }
    }
}

bool check_estimates_on_outputs(const vector<Function>& outputs) {
    bool estimates_avail = true;
    for (auto& out : outputs) {
        const vector<Bound>& estimates = out.schedule().estimates();
        if (estimates.size() != out.args().size()) {
            estimates_avail = false;
            break;
        }
        vector<string> vars = out.args();

        for (unsigned int i = 0; i < estimates.size(); i++) {
            if (std::find(vars.begin(), vars.end(), estimates[i].var) == vars.end()
                || !((estimates[i].min.as<IntImm>()) &&
                     (estimates[i].extent.as<IntImm>())))  {
                estimates_avail = false;
                break;
            }
        }
    }
    return estimates_avail;
}

struct MachineParams {
    unsigned int parallelism;
    unsigned int vec_len;
    unsigned int fast_mem_size;
    unsigned int balance;
};

struct CostModel {

    /* Visitor for computing the arithmetic cost of a single value of a function*/
    const map<string, Function> &env;
    map<string, pair<int64_t, int64_t> > func_cost;
    map<string, Type> inputs;

    class ExprCost : public IRVisitor {
     public:
      int ops;
      int byte_loads;

      ExprCost() {
          ops = 0; byte_loads = 0;
      }

      using IRVisitor::visit;

      void visit(const IntImm *) {}
      void visit(const UIntImm *) {}
      void visit(const FloatImm *) {}
      void visit(const StringImm *) {}
      void visit(const Cast * op) {
          op->value.accept(this);
          ops+=1;
      }
      void visit(const Variable *) {}

      template<typename T>
              void visit_binary_operator(const T *op, int cost) {
                  op->a.accept(this);
                  op->b.accept(this);
                  ops += cost;
              }

      // TODO: Figure out the right costs
      void visit(const Add *op) {visit_binary_operator(op, 1);}
      void visit(const Sub *op) {visit_binary_operator(op, 1);}
      void visit(const Mul *op) {visit_binary_operator(op, 1);}
      void visit(const Div *op) {visit_binary_operator(op, 1);}
      void visit(const Mod *op) {visit_binary_operator(op, 1);}
      void visit(const Min *op) {visit_binary_operator(op, 1);}
      void visit(const Max *op) {visit_binary_operator(op, 1);}
      void visit(const EQ *op) {visit_binary_operator(op, 1);}
      void visit(const NE *op) {visit_binary_operator(op, 1);}
      void visit(const LT *op) {visit_binary_operator(op, 1);}
      void visit(const LE *op) {visit_binary_operator(op, 1);}
      void visit(const GT *op) {visit_binary_operator(op, 1);}
      void visit(const GE *op) {visit_binary_operator(op, 1);}
      void visit(const And *op) {visit_binary_operator(op, 1);}
      void visit(const Or *op) {visit_binary_operator(op, 1);}

      void visit(const Not *op) {
          op->a.accept(this);
          ops+=1;
      }

      void visit(const Select *op) {
          op->condition.accept(this);
          op->true_value.accept(this);
          op->false_value.accept(this);
          ops+=1;
      }

      void visit(const Call * call) {
          if (call->call_type == Call::Halide ||
              call->call_type == Call::Image) {
              byte_loads += call->type.bytes();
          } else if (call->call_type == Call::Extern) {
              // There is no visibility into an extern stage so there is
              // no way to know the cost of the call statically. This may
              // require profiling or user annotation
              //
              // For now making this a large constant so that functions with
              // extern stages are forced to be compute_root
              ops+=999;
          } else if (call->call_type == Call::Intrinsic) {
              // TODO: Figure out the right costs based on intrinsic type
              ops+=1;
              // TODO: There is a PureIntrinsic too figure out what it is
              // and how to cost it
          }

          for (size_t i = 0; (i < call->args.size()); i++)
              call->args[i].accept(this);
      }

      void visit(const Let * let) {
          let->value.accept(this);
          let->body.accept(this);
      }

      // Should not hit any of these IR nodes at this stage of compilation
      void visit(const Load *) { internal_assert(0); }
      void visit(const Ramp *) { internal_assert(0); }
      void visit(const Broadcast *) { internal_assert(0); }
      void visit(const LetStmt *) { internal_assert(0); }
      void visit(const AssertStmt *) {}
      void visit(const ProducerConsumer *) { internal_assert(0); }
      void visit(const For *) { internal_assert(0); }
      void visit(const Store *) { internal_assert(0); }
      void visit(const Provide *) { internal_assert(0); }
      void visit(const Allocate *) { internal_assert(0); }
      void visit(const Free *) { internal_assert(0); }
      void visit(const Realize *) { internal_assert(0); }
      void visit(const Block *) { internal_assert(0); }
      void visit(const IfThenElse *) { internal_assert(0); }
      void visit(const Evaluate *) { internal_assert(0); }
    };

    Expr perform_inline(Expr e, const set<string> &inlines = set<string>()) {

        if (inlines.empty())
            return e;

        bool funcs_to_inline = false;
        Expr inlined_expr = e;

        do {
            funcs_to_inline = false;
            FindAllCalls find;
            inlined_expr.accept(&find);
            set<string>& calls = find.calls;
            for (auto& call: calls) {
                if (inlines.find(call) != inlines.end() && env.at(call).is_pure()) {
                    funcs_to_inline = true;
                    inlined_expr = inline_function(inlined_expr, env.at(call));
                    break;
                }
            }
        } while (funcs_to_inline);

        /*
        ExprCost cost;
        e.accept(&cost);
        debug(0) << "Original:" << e << "," << cost.ops << '\n';

        ExprCost cost_inlined;
        inlined_expr.accept(&cost_inlined);
        debug(0) << "Inlined:" << inlined_expr << "," << cost_inlined.ops << '\n';
        */

        return inlined_expr;
    }

    pair<int, int> get_expr_cost(Expr& e) {
        ExprCost cost_visitor;
        e.accept(&cost_visitor);
        return make_pair(cost_visitor.ops, cost_visitor.byte_loads);
    }

    pair<int64_t, int64_t> region_cost(string func, Box& region,
                                       const set<string>& inlines = set<string>()) {

        int64_t area = box_area(region);
        if (area < 0) {
            // Area could not be determined therfore it is not possible to determine
            // the cost as well
            return make_pair(-1, -1);
        }

        pair<int64_t, int64_t> cost = inlines.empty() ? func_cost[func]:
                get_func_cost(env.at(func), inlines);

        cost.first *= area;
        cost.second *= area;

        internal_assert(cost.first >= 0 && cost.second >=0);
        return cost;
    }

    pair<int64_t, int64_t> region_cost(map<string, Box>& regions,
                                       const set<string>& inlines = set<string>()) {

        pair<int64_t, int64_t> total_cost(0, 0);
        for (auto& f: regions) {
            // The cost for inlined functions will be accounted in the consumer
            // of the inlined function
            if (inlines.find(f.first) != inlines.end() &&
                env.at(f.first).is_pure())
                continue;

            pair<int64_t, int64_t> cost = region_cost(f.first, f.second, inlines);
            if (cost.first < 0) {
                return cost;
            }
            else {
                total_cost.first += cost.first;
                total_cost.second += cost.second;
            }
        }

        internal_assert(total_cost.first >= 0 && total_cost.second >=0);
        return total_cost;
    }

    pair<int64_t, int64_t> get_func_cost(const Function& f,
                                         const set<string>& inlines = set<string>()) {

        int64_t total_ops = 1;
        int64_t total_loads = 0;
        // TODO: revist how boundary conditions are handled
        for (auto& e: f.values()) {
            Expr inlined_expr = perform_inline(e, inlines);
            ExprCost cost_visitor;
            inlined_expr.accept(&cost_visitor);
            total_ops += cost_visitor.ops;
            total_loads += cost_visitor.byte_loads;
        }

        // Estimating cost when reductions are involved
        // TODO: This assumes that the entire reduction of each update definition is
        // evaluated to compute each value of the function.
        if (!f.is_pure()) {
            for (const Definition& u: f.updates()) {

                int64_t ops = 1;
                int64_t loads = 0;
                for (auto& e: u.values()) {
                    Expr inlined_expr = perform_inline(e, inlines);
                    ExprCost cost_visitor;
                    inlined_expr.accept(&cost_visitor);
                    ops += cost_visitor.ops;
                    loads += cost_visitor.byte_loads;
                }

                for (auto& arg: u.args()) {
                    Expr inlined_arg = perform_inline(arg, inlines);
                    ExprCost cost_visitor;
                    inlined_arg.accept(&cost_visitor);
                    ops += cost_visitor.ops;
                    loads += cost_visitor.byte_loads;
                }

                if (u.domain().defined()) {
                    Box b;
                    for (auto& rvar: u.domain().domain()) {
                        b.push_back(Interval(simplify(rvar.min),
                                             simplify(rvar.min + rvar.extent - 1)));
                    }
                    int64_t area = box_area(b);
                    if (area != -1) {
                        total_ops += ops * area;
                        total_loads += loads * area;
                    } else {
                        total_ops = -1;
                        total_loads = -1;
                        debug(0) << "Warning: could not determine the bounds of\
                                 the rdom in function " << f.name() << '\n';
                    }
                }
            }
        }
        return make_pair(total_ops, total_loads);
    }

    int64_t get_func_value_size(const Function& f) {
        int64_t size = 0;
        const vector<Type>& types = f.output_types();
        for (size_t i = 0; i < types.size(); i++)
            size += types[i].bytes();
        internal_assert(types.size() != 0);
        return size;
    }

    int64_t region_size(string func, const Box &region) {
        const Function& f = env.at(func);
        int64_t area = box_area(region);
        if (area < 0) {
            // Area could not be determined
            return -1;
        }
        int64_t size_per_ele = get_func_value_size(f);
        return area * size_per_ele;
    }

    int64_t region_size(const map<string, Box>& regions,
                        const set<string>& inlined = set<string>()) {

        map<string, int> num_consumers;
        for (auto& f: regions)
            num_consumers[f.first] = 0;

        for (auto& f: regions) {
            map<string, Function> prods = find_direct_calls(env.at(f.first));
            for (auto& p: prods) {
                if (regions.find(p.first) != regions.end())
                    num_consumers[p.first] += 1;
            }
        }

        vector<Function> outs;
        for (auto &f: num_consumers) {
            if (f.second    == 0) {
                outs.push_back(env.at(f.first));
            }
        }

        // Realization order
        vector<string> order = realization_order(outs, env);

        int64_t working_set_size = 0;
        int64_t curr_size = 0;

        map<string, int64_t> func_sizes;

        for (auto& f: regions) {
            // Inlined functions do not have allocations
            int64_t size = inlined.find(f.first) != inlined.end()? 0:
                    region_size(f.first, f.second);
            if (size < 0)
                return -1;
            else
                func_sizes[f.first] = size;
        }

        for (auto& f: order) {
            if (regions.find(f) != regions.end()) {
                curr_size += func_sizes.at(f);
            }
            working_set_size = std::max(curr_size, working_set_size);
            map<string, Function> prods = find_direct_calls(env.at(f));
            for (auto& p: prods) {
                if (num_consumers.find(p.first) != num_consumers.end()) {
                    num_consumers[p.first] -= 1;
                    if (num_consumers[p.first] == 0) {
                        curr_size -= func_sizes.at(p.first);
                        internal_assert(curr_size >= 0);
                    }
                }
            }
        }

        return working_set_size;
    }

    int64_t input_region_size(string input, const Box &region) {
        int64_t area = box_area(region);
        if (area < 0) {
            // Area could not be determined
            return -1;
        }
        int64_t size_per_ele = inputs.at(input).bytes();
        return area * size_per_ele;
    }

    int64_t input_region_size(const map<string, Box>& input_regions) {
        int64_t total_size = 0;
        for (auto& reg: input_regions) {
            int64_t size = input_region_size(reg.first, reg.second);
            if (size < 0) {
                return -1;
            } else {
                total_size += size;
            }
        }
        return total_size;
    }

    CostModel(const map<string, Function>& _env) : env(_env) {
        for (auto& kv : env) {
            func_cost[kv.first] = get_func_cost(kv.second);
            FindImageInputs find;
            kv.second.accept(&find);
            for (auto& in: find.input_type) {
                inputs[in.first] = in.second;
            }
        }
    }
};

struct DependenceAnalysis {

    const map<string, Function>& env;
    const FuncValueBounds& func_val_bounds;

    // TODO: Build a cache for bounds queries

    DependenceAnalysis(map<string, Function>& _env,
                       const FuncValueBounds& _func_val_bounds):
        env(_env), func_val_bounds(_func_val_bounds) {}

    void simplify_box(Box& b) {
        for (unsigned int i = 0; i < b.size(); i++) {
            b[i].min = simplify(b[i].min);
            b[i].max = simplify(b[i].max);
        }
    }

    // Compute the regions of producers required to compute a region of the
    // function 'f' given concrete sizes of the tile in each dimension.
    map<string, Box> regions_required(Function f,
                                      const vector<Interval>& conc_bounds) {

        map<string, Box> regions;
        // Add the function and its region to the queue
        deque< pair<Function, vector<Interval> > > f_queue;
        f_queue.push_back(make_pair(f, conc_bounds));

        // Recursively compute the regions required
        while(!f_queue.empty()) {

            Function curr_f = f_queue.front().first;
            vector<Interval> curr_bounds = f_queue.front().second;
            f_queue.pop_front();

            for (auto& val: curr_f.values()) {
                map<string, Box> curr_regions;
                Scope<Interval> curr_scope;
                int interval_index = 0;

                for (auto& arg: curr_f.args()) {
                    Interval simple_bounds = 
                            Interval(simplify(curr_bounds[interval_index].min),
                                     simplify(curr_bounds[interval_index].max));
                    curr_scope.push(arg, simple_bounds);
                    interval_index++;
                }

                curr_regions = boxes_required(val, curr_scope, func_val_bounds);
                for (auto& reg: curr_regions) {
                    // merge region with an existing region for the function in the global
                    // map
                    if (regions.find(reg.first) == regions.end())
                        regions[reg.first] = reg.second;
                    else
                        merge_boxes(regions[reg.first], reg.second);

                    if (env.find(reg.first) != env.end())
                        f_queue.push_back(make_pair(env.at(reg.first),
                                                    reg.second.bounds));
                }
            }

            // TODO: Check if this handling of updates is correct
            for (auto& update: curr_f.updates()) {
                for (auto& val: update.values()) {
                    map<string, Box> curr_regions;
                    Scope<Interval> curr_scope;
                    int interval_index = 0;
                    vector<Expr> exprs;
                    exprs.push_back(val);
                    for (auto& arg: update.args()) {
                        Interval simple_bounds = 
                                Interval(simplify(curr_bounds[interval_index].min),
                                         simplify(curr_bounds[interval_index].max));                    
                        // Check for a pure variable
                        const Variable *v = arg.as<Variable>();
                        if (!v) {
                            // Need to evaluate boxes required on args that are not pure for
                            // potenial calls to other functions
                            exprs.push_back(arg);
                        } else {
                            curr_scope.push(v->name, simple_bounds);
                        }
                        interval_index++;
                    }

                    if (update.domain().defined()) {
                        for (auto& rvar: update.domain().domain()) {
                            Interval simple_bounds = Interval(rvar.min,
                                                              rvar.min + rvar.extent - 1);
                            curr_scope.push(rvar.var, simple_bounds);
                        }
                    }

                    for (auto& e: exprs) {
                        curr_regions = boxes_required(e, curr_scope, func_val_bounds);
                        for (auto& reg: curr_regions) {
                            // Merge region with an existing region for the function in
                            // the global map
                            if(reg.first != curr_f.name()) {
                                if (regions.find(reg.first) == regions.end())
                                    regions[reg.first] = reg.second;
                                else
                                    merge_boxes(regions[reg.first], reg.second);

                                if (env.find(reg.first) != env.end())
                                    f_queue.push_back(make_pair(env.at(reg.first),
                                                                reg.second.bounds));
                            }
                        }
                    }
                }
            }
        }

        // Simplify
        map<string, Box> concrete_regions;

        for (auto& f_reg : regions) {
            simplify_box(f_reg.second);

            Box concrete_box;
            for (unsigned int i = 0; i < f_reg.second.size(); i++) {
                Expr lower = f_reg.second[i].min;
                Expr upper = f_reg.second[i].max;

                // TODO: Assumes estimates cannot be provided on input parameters like
                // images. Need to have a better way of doing this where input
                // parameters can have estimates attached to them.
                //
                // Also make the simplification take them into account.
                bool in_env = (env.find(f_reg.first) != env.end());

                // Use the estimates if the lower and upper bounds cannot be determined
                if (!lower.as<IntImm>() && in_env) {
                    const Function& curr_f = env.at(f_reg.first);
                    for (auto& b: curr_f.schedule().estimates()) {
                        unsigned int num_pure_args = curr_f.args().size();
                        if (i < num_pure_args && b.var == curr_f.args()[i])
                            lower = Expr(b.min.as<IntImm>()->value);
                    }
                }

                if (!upper.as<IntImm>() && in_env) {
                    const Function& curr_f = env.at(f_reg.first);
                    for (auto& b: curr_f.schedule().estimates()) {
                        unsigned int num_pure_args = curr_f.args().size();
                        if (i < num_pure_args && b.var == curr_f.args()[i]) {
                            const IntImm * bmin = b.min.as<IntImm>();
                            const IntImm * bextent = b.extent.as<IntImm>();
                            upper = Expr(bmin->value + bextent->value - 1);
                        }
                    }
                }

                Interval concrete_bounds = Interval(lower, upper);
                concrete_box.push_back(concrete_bounds);
            }
            concrete_regions[f_reg.first] = concrete_box;
        }
        return concrete_regions;
    }

    // Compute the redundant regions computed while computing a tile of the
    // function 'f' given sizes of the tile in each dimension.
    map<string, Box> redundant_regions(Function f, int dir,
                                       const vector<Interval>& conc_bounds) {

        map<string, Box> regions = regions_required(f, conc_bounds);

        vector<Interval> shifted_bounds;
        int num_pure_args = f.args().size();
        for (int arg = 0; arg < num_pure_args; arg++) {
            if (dir == arg) {
                Expr len = conc_bounds[arg].max - conc_bounds[arg].min + 1;
                Interval bound = Interval(conc_bounds[arg].min + len,
                                          conc_bounds[arg].max + len);
                shifted_bounds.push_back(bound);
            }
            else
                shifted_bounds.push_back(conc_bounds[arg]);
        }

        map<string, Box> regions_shifted = regions_required(f, shifted_bounds);

        map<string, Box> overalps;
        for (auto& reg: regions) {
            if (regions_shifted.find(reg.first) == regions.end()) {
                // It will be interesting to log cases where this actually happens
                continue;
            } else {
                Box b = reg.second;
                Box b_shifted = regions_shifted[reg.first];
                // The boxes should be of the same size
                internal_assert(b.size() == b_shifted.size());
                // The box used makes things complicated ignoring it for now
                Box b_intersect;
                for (unsigned int i = 0 ; i < b.size(); i++)
                    b_intersect.push_back(interval_intersect(b[i], b_shifted[i]));
                // A function should appear once in the regions and therefore cannot
                // already be present in the overlaps map
                internal_assert(overalps.find(reg.first) == overalps.end());
                overalps[reg.first] = b_intersect;
            }
        }
        // Simplify
        for (auto& f : overalps)
            simplify_box(f.second);

        return overalps;
    }


    map<string, Box>
            concrete_dep_regions(string name, vector<Interval>& bounds) {
                return regions_required(env.at(name), bounds);
            }

    vector< map<string, Box> >
            concrete_overlap_regions(string name, vector<Interval>& bounds) {

                vector< map<string, Box> > conc_overlaps;
                size_t num_args = env.at(name).args().size();

                for (size_t dir = 0; dir < num_args; dir++) {
                    map<string, Box> conc_reg =
                            redundant_regions(env.at(name), dir, bounds);
                    conc_overlaps.push_back(conc_reg);
                }
                return conc_overlaps;
            }
};

void disp_regions(map<string, Box>& regions) {
    for (auto& reg: regions) {
        debug(0) << reg.first;
        debug(0) << reg.second;
        debug(0) << "\n";
    }
}

map<string, Box> get_pipeline_bounds(DependenceAnalysis& analy,
                                     const vector<Function>& outputs) {
    map<string, Box> pipeline_bounds;

    for (auto& out: outputs) {
        vector<Interval> bounds;
        vector<string> vars = out.args();
        for (size_t i = 0; i < vars.size(); i++) {
            for (auto& b: out.schedule().estimates())
                if (b.var == vars[i]) {
                    Interval I = Interval(b.min, simplify(b.min + b.extent - 1));
                    bounds.push_back(I);
                }
        }

        map<string, Box> regions =
                analy.concrete_dep_regions(out.name(), bounds);

        // Add the output region to the pipeline bounds as well
        regions[out.name()] = bounds;

        for (auto& reg: regions) {
            // Merge region with an existing region for the function in the global map
            if (pipeline_bounds.find(reg.first) == pipeline_bounds.end())
                pipeline_bounds[reg.first] = reg.second;
            else
                merge_boxes(pipeline_bounds[reg.first], reg.second);
        }
    }

    return pipeline_bounds;
}

struct Partitioner {

    struct InlineChoice {
        // InlineChoice encodes the choice of the prod_group being inlined into the
        // cons_group
        string prod_group;
        string cons_group;
        InlineChoice(string _prod_group, string _cons_group) :
            prod_group(_prod_group), cons_group(_cons_group) {}
    };

    struct FusionChoice {
        // FusionChoice encodes the choice of the prod_group being merged with
        // the cons_group at the granularity of the tile given by tile_sizes
        string prod_group;
        string cons_group;
        // Tile sizes along the output of the consumer group
        map<string, int> tile_sizes;

        FusionChoice(string _prod_group, string _cons_group,
                     map<string, int>& _tile_sizes) :
            prod_group(_prod_group), cons_group(_cons_group),
            tile_sizes(_tile_sizes) {}

        friend std::ostream& operator <<(std::ostream& stream,
                                      const FusionChoice& choice) {

            stream << "Choice:" << choice.prod_group << "->"
                   << choice.cons_group << '\n';

            stream << "Tile sizes:" << "[";
            for (auto& s: choice.tile_sizes) {
                stream << "(" << s.first << "," <<  s.second << ")";
            }
            stream << "]" << '\n';

            return stream;
        }

    };

    map<pair<string, string>, pair<InlineChoice, int64_t>> inline_cache;
    map<pair<string, string>, pair<FusionChoice, int64_t> > fusion_cache;

    struct Group {
        // The output function representing the group
        string output;
        // All the functions that belong to the group
        vector<Function> members;

        // Reuse along dimensions of the group members
        map<string, map<string, int64_t> > reuse;

        // Schedule information
        // All the members of the group which are inlined
        set<string> inlined;
        // For now this is just the tile sizes since the we only tile the output of
        // the group and compute all the members of the group at that granularity
        map<string, int> tile_sizes;

        Group(string _output, vector<Function> _members):
              output(_output), members(_members) { }

        friend std::ostream& operator <<(std::ostream& stream, const Group& g) {

            stream << "Output:" << g.output << '\n';
            stream << "Members:" << '[';
            for (auto& m: g.members) {
                stream << m.name() << ",";
            }
            stream << "]" << '\n';

            stream << "Inlined:" << '[';
            for (auto& in: g.inlined) {
                stream << in << ",";
            }
            stream << "]" << '\n';

            stream << "Tile sizes:" << "[";
            for (auto& s: g.tile_sizes) {
                stream << "(" << s.first << "," <<  s.second << ")";
            }
            stream << "]" << '\n';

            return stream;
        }
    };

    struct GroupAnalysis {
        // Estimate of arithmetic cost
        int64_t arith_cost;
        // Estimate of accesses to slow memory
        int64_t mem_cost;
        // Estimate of the parallelism
        int64_t parallelism;
    };

    map<string, Group> groups;

    // Levels that are targetted by the grouping algorithm
    enum Level {INLINE, FAST_MEM};

    map<string, Box>& pipeline_bounds;
    MachineParams& arch_params;
    DependenceAnalysis& analy;
    CostModel& cost_model;
    const vector<Function>& outputs;

    map<string, set<string> > children;
    void disp_children(map<string, set<string> >& children) {
        for (auto& f: children) {
            debug(0) << f.first <<  ": [";
            for (auto& c: f.second)
                debug(0) << c << ",";
            debug(0) << "]" << '\n';
        }
    }

    bool gpu_schedule;

    Partitioner(map<string, Box>& _pipeline_bounds, MachineParams& _arch_params,
                DependenceAnalysis& _analy, CostModel& _cost_model,
                const vector<Function>& _outputs, bool _gpu_schedule):
        pipeline_bounds(_pipeline_bounds), arch_params(_arch_params),
        analy(_analy), cost_model(_cost_model), outputs(_outputs),
        gpu_schedule(_gpu_schedule) {

            // Place each function in its own group
            for (auto& f: analy.env) {
                vector<Function> members;
                members.push_back(f.second);
                Group g(f.first, members);
                groups.insert(make_pair(f.first, g));
            }

            // Find consumers of each function and relate groups with their children
            for (auto& f: analy.env) {
                map<string, Function> calls = find_direct_calls(f.second);
                for (auto& c: calls)
                    if (c.first != f.first)
                        children[c.first].insert(f.first);
            }

            disp_children(children);

            // TODO: Any preprocess inlining should go here and they should be added to
            // the corresponding group as inlined members

            // TODO: FindAllCalls might be unnecessary and it probably can be
            // replaced by find_direct_calls

        }

    void merge_groups(FusionChoice& choice) {

        string cand_group = choice.prod_group;
        string child_group = choice.cons_group;

        internal_assert(groups.find(child_group) != groups.end());
        vector<Function> cand_funcs = groups.at(cand_group).members;

        groups.erase(cand_group);

        vector<Function>& child_members = groups.at(child_group).members;
        child_members.insert(child_members.end(),
                             cand_funcs.begin(), cand_funcs.end());

        // TODO: Look at all the members that need to be to be updated. Maybe merge
        // should be a member of the group class so that it is more contained.
        groups.at(child_group).tile_sizes = choice.tile_sizes;

        // Update the children mapping
        children.erase(cand_group);
        for (auto& f: children) {
            set<string>& cons = f.second;
            if (cons.find(cand_group) != cons.end()) {
                cons.erase(cand_group);
                cons.insert(child_group);
            }
        }
    }

    void merge_group(InlineChoice& choice) {

        string cand_group = choice.prod_group;

        set<string> cand_group_children = children[cand_group];
        for (auto& cg: cand_group_children) {
            internal_assert(groups.find(cg) != groups.end());
            vector<Function> cand_funcs = groups.at(cand_group).members;

            vector<Function>& cg_members = groups.at(cg).members;
            cg_members.insert(cg_members.end(),
                              cand_funcs.begin(), cand_funcs.end());
            // TODO: Look at all the members that need to be to be updated. Maybe
            // merge should be a member of the group class so that it is more
            // contained.
            groups.at(cg).inlined.insert(cand_group);
        }

        groups.erase(cand_group);

        // Update the children mapping
        children.erase(cand_group);
        for (auto& f: children) {
            set<string>& cons = f.second;
            if (cons.find(cand_group) != cons.end()) {
                cons.erase(cand_group);
                cons.insert(cand_group_children.begin(),
                            cand_group_children.end());
            }
        }
    }

    void disp_grouping() {
        for (auto& g: groups) {
            debug(0) << "Group " <<  g.first    << " : [" ;
            debug(0) << g.second;
            debug(0) << "]" << '\n';
        }
    }

    int64_t evaluate_choice(FusionChoice& choice);
    int64_t evaluate_choice(InlineChoice& choice);

    Group fuse_groups(Group& g1, Group& g2);

    GroupAnalysis analyze_group(const Group& g);
    vector<Interval> get_bounds_from_tile_sizes(const string& func,
                                                const map<string, int>& tile_sizes);
    map<string, Box> get_group_member_bounds(const Group& g);
    void group(Partitioner::Level level);
    pair<vector<InlineChoice>, int64_t>
            choose_candidate_fuse_inline(const vector< pair<string, string > >& cand_pairs);
    pair<FusionChoice, int64_t>
            choose_candidate_fuse_fast_mem(const vector< pair<string, string > >& cand_pairs);
    map<string, int64_t> evaluate_reuse(string func, const set<string>& prod);
    map<string, int> get_pure_dim_estimates(const string& f);
    string generate_cpu_schedule(const Target& t);
    string generate_group_cpu_schedule(const Group& g, const Target& t);
    /*
       Option choose_candidate(const vector< pair<string, string > >& cand_pairs);
       void clear_schedules_fast_mem();
       void initialize_groups_fast_mem();
       void initialize_groups_inline();
       void update_function_costs();
       void tile_for_input_locality(bool init_pipeline_reuse = false);
       vector<float> get_input_reuse(Function f, vector<string>& inputs);
       */
};

map<string, int64_t> Partitioner::evaluate_reuse(string func,
                                                 const set<string>& prod) {
    map<string, int64_t> reuse;
    Function f = analy.env.at(func);
    const vector<string>& args = f.args();

    map<string, int> tile_sizes;
    // TODO: Check if tile sizes of 1 in each dimension gives a reasonable answer
    // or reuse should be evaluated at a much larger granularity or symbolically.
    // Using a symbolic version might be better if the objective is to find
    // dimensions with no reuse. The only downside with the symbolic method is it
    // totally at the mercy of the simplifier.  Another option is sampling or
    // using a larger granularity
    for (size_t i = 0; i < args.size(); i++)
        tile_sizes[args[i]] = 1;

    vector<Interval> bounds = get_bounds_from_tile_sizes(func, tile_sizes);

    vector< map<string, Box> > reuse_regions =
            analy.concrete_overlap_regions(func, bounds);

    for (size_t i = 0; i < args.size(); i++) {
        int64_t total_reuse = 0;
        for (auto& reg: reuse_regions[i]) {
            // Discard all the regions not in procucer set
            if (prod.find(reg.first) == prod.end())
                continue;
            int64_t area = box_area(reg.second);
            if (area >= 0) {
                total_reuse += area;
            } else {
                total_reuse = -1;
                break;
            }
        }
        reuse[args[i]] = total_reuse;
    }

    return reuse;
}

pair<vector<Partitioner::InlineChoice>, int64_t>
Partitioner::choose_candidate_fuse_inline(
        const vector<pair<string, string> >& cands) {

    pair<vector<Partitioner::InlineChoice>, int64_t> best;
    best.second = -1;
    for (auto& p: cands) {
        internal_assert(p.second == "");
        // Compute the aggregate benefit for inlining into all the children
        int64_t overall_benefit = 0;
        vector<Partitioner::InlineChoice> choices;

        for (auto& c: children[p.first]) {
            int64_t benefit = 0;
            InlineChoice cand_choice(p.first, c);

            pair<string, string> prod_cons = make_pair(p.first, c);
            // Check if the pair has been evaluated for inline fusion before
            if (inline_cache.find(prod_cons) != inline_cache.end()) {

                benefit = inline_cache.at(prod_cons).second;

            } else {
                benefit = evaluate_choice(cand_choice);
                // Cache the result of the evaluation for the pair
                inline_cache.insert(make_pair(prod_cons,
                                              make_pair(cand_choice, benefit)));
            }

            // Conservative strategy that only goes ahead with the fusion if all the
            // fusions into the consumers are beneficial
            // TODO: Create a test where this assumption breaks
            if (benefit < 0) {
                overall_benefit = -1;
                choices.clear();
                break;
            } else {
                choices.push_back(cand_choice);
                overall_benefit += benefit;
            }
        }

        if (best.second < overall_benefit
            /*||
              (best.second == overall_benefit &&
              best.first[0].prod_group > p.first)*/) {
            best.first = choices;
            best.second = overall_benefit;
        }
    }
    return best;
}

pair<Partitioner::FusionChoice, int64_t>
Partitioner::choose_candidate_fuse_fast_mem(
        const vector< pair<string, string> >& cand_pairs) {

    map<string, int> tile_sizes;
    FusionChoice c("", "", tile_sizes);
    return make_pair(c, 0);

    // The choose candidate operates by considering a wide variety of
    // posssible fusion structures between each pair of candidates. The fusion
    // structure is restricted to computing all the functions in both the
    // groups at some granularity of the output function in the child group.
    //
    // Among these options the only ones considered are the ones that satisfy
    // the machine constraints. This means the following things:
    //
    // 1) Do all the intermediate buffers fit in the fast level of memory. One
    // needs to account for early frees and the high watermark of intermediate
    // storage.
    //
    // 2) Is the amount of redundant computation introduced in the process
    // give the best redundant compute vs. locality trade-off.
    //
    // 3) Does the fused group have enough parallelism both for multiple cores.
    // This can get tricky as it has load balancing aspect to it too. For
    // example, if the group can be split into 10 tiles and there are 4 cores the
    // latency of the entire pipeline is 3 tiles. So either the number of tiles
    // have to a multiple of the cores or large in number to avoid the load
    // imbalance.
    //
    // 4) Does the fusion limit vectorization. Reordering function dimensions
    // and modifying data layout have significant interactions with
    // vectorization. As a first pass the goal is to not miss any obvious
    // vectorization.
}

void Partitioner::group(Partitioner::Level level) {
    // Partition the pipeline by iteratively merging groups until a fixpoint
    bool fixpoint = false;
    while(!fixpoint) {
        fixpoint = true;
        vector< pair<string, string> > cand;
        for (auto& g: groups) {

            bool is_output = false;
            for (auto& f: outputs) {
                if (g.first == f.name())
                    is_output = true;
            }

            if (is_output)
                continue;

            if (children.find(g.first) != children.end()) {
                int num_children = children[g.first].size();
                // Find all the groups which have a single child
                if (num_children == 1 && level == Partitioner::FAST_MEM) {
                    cand.push_back(make_pair(g.first,
                                             *children[g.first].begin()));
                } else if(num_children > 0  && level == Partitioner::INLINE) {
                    cand.push_back(make_pair(g.first, ""));
                }
            }
        }

        debug(0) << "Current grouping candidates:" << '\n';
        for (auto& p: cand) {
            debug(0) << "[" << p.first << "," <<    p.second << "]" << '\n';
        }

        vector<pair<string, string> > invalid_keys;
        if (level == Partitioner::INLINE) {
            pair<vector<InlineChoice>, int64_t> best;
            best = choose_candidate_fuse_inline(cand);
            if (best.second >= 0) {
                string prod = best.first[0].prod_group;

                // Mark the entries of the fusion cache that need to be
                // invalidated
                for (auto& c: children.at(prod)) {
                    for (auto& choice: inline_cache) {
                        if (choice.first.first == c ||
                            choice.first.second == c)
                            invalid_keys.push_back(choice.first);
                    }
                }

                for (auto& inline_choice: best.first) {
                    internal_assert(inline_choice.prod_group == prod);
                    merge_group(inline_choice);
                }

                fixpoint = false;
            }
            // Invalidate the inline cache
            for (auto& key: invalid_keys)
                inline_cache.erase(key);

        } else {
            pair<FusionChoice, int64_t> best
                    = choose_candidate_fuse_fast_mem(cand);
            if (best.second >= 0) {

                // Mark the entries of the fusion cache that need to be
                // invalidated
                for (auto& choice: fusion_cache) {
                    if (choice.first.second == best.first.cons_group
                        || choice.first.first == best.first.cons_group)
                        invalid_keys.push_back(choice.first);
                }

                // Invalidate the fusion cache
                for (auto& key: invalid_keys)
                    fusion_cache.erase(key);

                merge_groups(best.first);
                fixpoint = false;
            }
        }
    }
}

vector<Interval> Partitioner::get_bounds_from_tile_sizes(const string& func,
                                                         const map<string, int>& tile_sizes) {
    vector<Interval> bounds;
    const vector<string>& args = analy.env.at(func).args();

    for (size_t i = 0; i < args.size(); i++) {
        if (tile_sizes.find(args[i]) != tile_sizes.end()) {
            int size = tile_sizes.at(args[i]);
            // Check if the bounds allow for tiling with the given tile size
            // i.e., ensure atleast 2 tiles
            int extent = get_extent(pipeline_bounds.at(func)[i]);
            if (extent >= 2 * size) {
                // TODO: Maybe shift this to the center of the pipeline bound
                bounds.push_back(Interval(0, size - 1));
            }
            else {
                // If the dimension is too small do not tile it and set the
                // extent of the bounds to that of the dimension estimate
                bounds.push_back(pipeline_bounds.at(func)[i]);
            }
        }
        else {
            bounds.push_back(pipeline_bounds.at(func)[i]);
        }
    }

    return bounds;
}

Partitioner::GroupAnalysis Partitioner::analyze_group(const Group& g) {
    // Estimating the number of accesses to slow memory

    // 1) Assume all loads are a miss if the working set does not fit in cache.
    // This ignores any locality that results from the iteration order. This is
    // pretty aggresive in estimating the benefit of fusion.
    //
    // 2) Assume that the intermediates are loaded only once even if they do not
    // fit in cache. It is a pretty good model for pipelines which are streaming
    // in nature. This gives a conservative estimate of fusion benefit and does
    // not accurately capture scenarios where there is significant reuse.
    //
    // The actual number of accesses will inbetween 2) and 1) for now going with
    // model 1).
    //
    // TODO: Model needs to be refined further to account for spatial locality and
    // iteration order.

    set<string> group_inputs;
    set<string> group_mem;

    for (auto& f: g.members) {
        group_mem.insert(f.name());
        FindAllCalls find;
        f.accept(&find);
        for (auto& c: find.calls) {
            bool is_member = false;
            for (auto& m: g.members) {
                if (m.name() == c) {
                    is_member = true;
                    break;
                }
            }
            if (!is_member) {
                group_inputs.insert(c);
            }
        }
    }

    // Count the number of tiles
    uint64_t estimate_tiles = 1;
    uint64_t num_ele_per_tile = 1;

    const vector<string>& args = analy.env.at(g.output).args();

    for (size_t i = 0; i < args.size(); i++) {
        if (g.tile_sizes.find(args[i]) != g.tile_sizes.end()) {
            int size = g.tile_sizes.at(args[i]);
            int extent = get_extent(pipeline_bounds.at(g.output)[i]);
            estimate_tiles *= std::ceil((float)extent/size);
            num_ele_per_tile *= size;
        }
    }

    // Get the regions of the pipeline required to compute a tile of the group
    vector<Interval> bounds = get_bounds_from_tile_sizes(g.output,
                                                         g.tile_sizes);
    map<string, Box> conc_reg = analy.concrete_dep_regions(g.output, bounds);
    conc_reg[g.output] = Box(bounds);

    map<string, Box> group_reg, prod_reg, input_reg;

    // Filtering out regions that belong to the group and are input to the group
    for (auto& reg: conc_reg) {
        if (group_mem.find(reg.first) != group_mem.end()) {
            group_reg[reg.first] = reg.second;
        } else if (group_inputs.find(reg.first) != group_inputs.end()) {
            if (analy.env.find(reg.first) != analy.env.end())
                prod_reg[reg.first] = reg.second;
            else
                input_reg[reg.first] = reg.second;
        }
    }

    // Compute the cost of the region and the size of the intermediates
    pair<int64_t, int64_t> tile_cost = cost_model.region_cost(group_reg, g.inlined);
    int64_t tile_input_size = cost_model.region_size(prod_reg) +
            cost_model.input_region_size(input_reg);
    int64_t tile_intermediate_size = cost_model.region_size(group_reg, g.inlined);

    GroupAnalysis g_analy;
    g_analy.arith_cost = -1;
    g_analy.mem_cost = -1;
    g_analy.parallelism = -1;

    // The group could not be analyzed
    if (tile_cost.first < 0 || tile_cost.second < 0 || tile_input_size < 0 ||
        tile_intermediate_size < 0) {
        return g_analy;
    }

    int64_t per_tile_mem_cost = tile_input_size;
    int64_t per_tile_arith_cost = tile_cost.first;

    if (tile_intermediate_size > arch_params.fast_mem_size) {
        per_tile_mem_cost += tile_cost.second;
    }

    g_analy.arith_cost = per_tile_arith_cost * estimate_tiles;
    g_analy.mem_cost = per_tile_mem_cost * estimate_tiles;
    g_analy.parallelism = estimate_tiles;

    /*
    debug(0) << "group:" << g.output << '\n';
    debug(0) << "arith_cost:" << g_analy.arith_cost << '\n';
    debug(0) << "mem_cost:" << g_analy.mem_cost << '\n';
    debug(0) << "per_tile_arith_cost:" << per_tile_arith_cost << '\n';
    debug(0) << "per_tile_mem_cost:" << per_tile_mem_cost << '\n';
    debug(0) << "estimate_tiles:" << estimate_tiles << '\n';
    */

    return g_analy;
}

Partitioner::Group Partitioner::fuse_groups(Group& prod_group,
                                            Group& cons_group) {

    vector<Function> fused_members;
    for (auto& f: prod_group.members)
        fused_members.push_back(f);
    for (auto& f: cons_group.members)
        fused_members.push_back(f);

    Group fused_group(cons_group.output, fused_members);

    for (auto& f: prod_group.inlined)
        fused_group.inlined.insert(f);
    for (auto& f: cons_group.inlined)
        cons_group.inlined.insert(f);

    return fused_group;
}

int64_t Partitioner::evaluate_choice(InlineChoice& choice) {

    // Create a group that reflects the fusion choice and evaluate
    // the cost of the group
    Group prod_group = groups.at(choice.prod_group);
    Group cons_group = groups.at(choice.cons_group);

    Group fused_group = fuse_groups(prod_group, cons_group);

    // Set the tile sizes to one along all dimensions of the consumer
    // group
    map<string, int> tile_sizes;

    Function out_func = analy.env.at(cons_group.output);
    const vector<string>& args = out_func.args();
    for (auto& arg: args) {
        tile_sizes[arg] = 1;
    }

    fused_group.tile_sizes = tile_sizes;

    for (auto& f: prod_group.members)
        fused_group.inlined.insert(f.name());
    for (auto& f: cons_group.inlined)
        fused_group.inlined.insert(f);

    // Compare the cost with the costs of the groups without fusion
    GroupAnalysis prod_analy = analyze_group(prod_group);
    GroupAnalysis cons_analy = analyze_group(cons_group);
    GroupAnalysis fused_analy = analyze_group(fused_group);

    // Return the overall benefit of the choice
    // TODO: Use the arch params to compute total work
    int64_t benefit = prod_analy.arith_cost + cons_analy.arith_cost -
            fused_analy.arith_cost;
    /*
    debug(0) << "\nProd Group:\n" << prod_group << '\n';
    debug(0) << "Cons Group:\n" << cons_group;
    debug(0) << "Benefit:" << benefit << "\n\n";
    */

    return benefit;
}

int64_t Partitioner::evaluate_choice(FusionChoice& choice) {

    // Create a group that reflects the fusion choice and evaluate
    // the cost of the group
    Group prod_group = groups.at(choice.prod_group);
    Group cons_group = groups.at(choice.cons_group);

    Group fused_group = fuse_groups(prod_group, cons_group);

    fused_group.tile_sizes = choice.tile_sizes;

    for (auto& f: prod_group.inlined)
        fused_group.inlined.insert(f);
    for (auto& f: cons_group.inlined)
        fused_group.inlined.insert(f);

    // Compare the cost with the costs of the groups without fusion
    GroupAnalysis prod_analy = analyze_group(prod_group);
    GroupAnalysis cons_analy = analyze_group(cons_group);
    GroupAnalysis fused_analy = analyze_group(fused_group);

    // Return the overall benefit of the choice
    // TODO: Use the arch params to compute total work
    return prod_analy.arith_cost + cons_analy.arith_cost - fused_analy.arith_cost;
}

map<string, int> Partitioner::get_pure_dim_estimates(const string& f) {
    map<string, int> dim_estimates;
    const vector<string>& args = analy.env.at(f).args();
    const vector<Dim>& dims = analy.env.at(f).schedule().dims();
    for (size_t i = 0; i < args.size(); i++) {
        int estimate = get_extent(pipeline_bounds.at(f)[i]);
        dim_estimates[dims[i].var] = estimate;
    }
    return dim_estimates;
}

map<string, Box> Partitioner::get_group_member_bounds(const Group& g) {
    map<string, Box> mem_bounds;
    vector<Interval> bounds = get_bounds_from_tile_sizes(g.output,
                                                         g.tile_sizes);
    map<string, Box> conc_reg = analy.concrete_dep_regions(g.output, bounds);
    for (auto& f: g.members) {
        if (conc_reg.find(f.name()) != conc_reg.end()) {
            mem_bounds[f.name()] = conc_reg[f.name()];
        }
    }
    return mem_bounds;
}

pair<VarOrRVar, VarOrRVar>
split_dim(Func f_handle, VarOrRVar v, int factor,
          string in_suffix, string out_suffix,
          map<string, int> &estimates, string &sched) {
    // Create new variables for the split dimensions
    string arg_name = v.name();
    string inner_name = f_handle.name() + "_" + arg_name + in_suffix;
    string outer_name = f_handle.name() + "_" + arg_name + out_suffix;
    VarOrRVar inner(inner_name), outer(outer_name);

    sched += "Var " + inner_name + "(\"" + outer_name + "\")" + ";\n";
    sched += "Var " + outer_name + "(\"" + outer_name + "\")" + ";\n";

    f_handle.split(v, outer, inner, factor);

    sched += f_handle.name() +
            ".split(" + arg_name + ',' + outer_name + ',' + inner_name +
            ',' + std::to_string(factor) + ";\n";

    internal_assert(estimates.find(arg_name) != estimates.end());

    estimates[inner_name] = factor;
    estimates[outer_name] =
            std::ceil((float)estimates.at(arg_name)/factor);
    estimates.erase(arg_name);

    return make_pair(inner, outer);
}

string Partitioner::generate_group_cpu_schedule(const Group& g,
                                                const Target& t) {
    string sched = "";
    Function g_out = analy.env.at(g.output);

    // Get estimates of pipeline bounds
    map<string, int> org_out_estimates = get_pure_dim_estimates(g.output);
    map<string, int> out_estimates = org_out_estimates;

    map<string, Box> group_bounds = get_group_member_bounds(g);
    Func f_handle(g_out);
    f_handle.compute_root();
    sched += f_handle.name() + ".compute_root()" + ";\n";

    // Realize tiling and update the dimension estimates
    vector<VarOrRVar> outer_dims;
    vector<VarOrRVar> inner_dims;

    for (auto &v: f_handle.args()) {
        if (g.tile_sizes.find(v.name()) != g.tile_sizes.end()) {
            string arg_name = v.name();
            int tile_size = g.tile_sizes.at(arg_name);
            if (tile_size > 1) {
                pair<VarOrRVar, VarOrRVar> tile_vars =
                        split_dim(f_handle, v, tile_size, "_i", "_o",
                                  out_estimates, sched);

                inner_dims.push_back(tile_vars.first);
                outer_dims.push_back(tile_vars.second);

            } else {
                outer_dims.push_back(v);
            }
        } else {
            inner_dims.push_back(v);
        }
    }

    // Reorder the tile dimensions
    if (outer_dims.size() > 0) {
        vector<VarOrRVar> ordering;
        for (auto& v: outer_dims)
            ordering.push_back(v);
        for (auto& v: inner_dims)
            ordering.push_back(v);

        f_handle.reorder(ordering);

        string var_order = ordering[0].name();
        for (auto& v: ordering) {
            var_order += ',' + v.name();
        }
        sched += f_handle.name() + ".reorder(" + var_order + ");\n";
    }

    // Vectorize the innermost dimension
    // TODO: Explore scenarios where vectorizing an outer dimension
    // makes more sense
    Var vec_dim = f_handle.args()[0];
    if (out_estimates.find(vec_dim.name()) != out_estimates.end()) {
        // Set the vector length as the maximum of the values produced by a
        // function
        int vec_len = 0;
        for (auto& type: g_out.output_types()) {
            vec_len = std::max(vec_len, t.natural_vector_size(type));
        }
        if (out_estimates[vec_dim.name()] >= vec_len) {
            pair<VarOrRVar, VarOrRVar> vec_vars =
                    split_dim(f_handle, vec_dim, vec_len, "_vi", "_vo",
                              out_estimates, sched);
            f_handle.vectorize(vec_vars.first);
            sched += f_handle.name() + ".vectorize(" +
                     vec_vars.first.name() + ");\n";
        }
    }
    /*
       for (auto& f_inline: g.inlined) {
       Func inline_handle(analy.env.at(f_inline));
       inline_handle.compute_root();
       }*/
    /*

    int num_fused_dims = 0;
    int parallelism = part.arch_params.parallelism;

    //std::cerr << "Start vectorization pure dims "
    //                      <<  g_out.name() << std::endl;
    {
    // Vectorize first
    Schedule &s = g_out.schedule();
    if (check_dim_size(s, 0, part.arch_params.vec_len, out_estimates))
    simple_vectorize(g_out, out_estimates, 0, part.arch_params.vec_len);
    else if (check_dim_size(s, 0, 8, out_estimates))
    simple_vectorize(g_out, out_estimates, 0, 8);
    else if (check_dim_size(s, 0, 4, out_estimates))
    simple_vectorize(g_out, out_estimates, 0, 4);

    int outer_dim = -1;
    bool can_par = pick_dim_to_parallelize(g_out, out_estimates,
    parallelism, num_tile_dims,
    outer_dim, num_fused_dims);

    if (auto_par && outer_dim !=-1 && can_par)
    parallelize_dim(g_out.schedule(), outer_dim);
    }

    //std::cerr << "Finished pure dims "    <<  g_out.name() << std::endl;
    if (!g_out.is_pure()) {

    int num_updates = g_out.updates().size();
    for (int i = 0; i < num_updates; i ++) {
    // Start with fresh bounds estimates for each update
    map<string, int> out_up_estimates = org_out_estimates;

    Schedule &s = g_out.update_schedule(i);
    vector<Dim> &dims = s.dims();

    const UpdateDefinition &u = g_out.updates()[i];

    // Tiling includes reduction dimensions
    map<string, int> tile_sizes_update;
    vector<string> update_vars;

    unsigned int num_pure_dims = g_out.args().size();
    unsigned int num_red_dims = (dims.size() - 1) - num_pure_dims;

    map<string, float> var_reuse;
    if (sched.locality) {
    assert(part.analy.reductions.find(g_out.name()) !=
    part.analy.reductions.end());
    assert(sched.tile_sizes.size() ==
    num_pure_dims + num_red_dims);
    // The tile sizes for the reduction dimensions are at the end
    for(unsigned int i = 0; i < num_red_dims; i++) {
        var_reuse[dims[i].var] = sched.reuse[num_pure_dims + i];
        if (sched.tile_sizes[num_pure_dims + i] != -1) {
            update_vars.push_back(dims[i].var);
            tile_sizes_update[dims[i].var] =
                    sched.tile_sizes[num_pure_dims + i];
            //std::cerr << dims[i].var << " "
            //                  << sched.tile_sizes[num_pure_dims + i]
            //                  << std::endl;
        }
    }
}

if (sched.fusion || sched.locality) {
    if (sched.fusion)
        assert(sched.tile_sizes.size() == num_pure_dims);
    for(unsigned int i = 0; i < num_pure_dims; i++) {
        var_reuse[dims[num_red_dims + i].var] =
                sched.reuse[i];
        if (sched.tile_sizes[i] != -1) {
            update_vars.push_back(dims[num_red_dims + i].var);
            tile_sizes_update[dims[num_red_dims + i].var]
                    = sched.tile_sizes[i];
            //std::cerr << dims[num_red_dims + i].var << " "
            //                  << sched.tile_sizes[i] << std::endl;
        }
    }
}

// Determine which dimension if any can be moved inner most
// while not disrupting spatial locality both on inputs and
// outputs
string inner_dim =
get_spatially_coherent_innermost_dim(g_out, u);

if (sched.locality) {
    reorder_by_reuse(s, var_reuse, inner_dim,
                     out_up_estimates, 16);
    update_vars.clear();
    for (int v = 0; v < (int)dims.size() - 1; v++) {
        if (tile_sizes_update.find(dims[v].var) != tile_sizes_update.end())
            update_vars.push_back(dims[v].var);
    }
}

set<string> par_vars;
for(auto &v: update_vars) {
    int index = -1;
    for (int i = 0; i < (int)dims.size() - 1; i++) {
        if (dims[i].var == v) {
            index = i;
            break;
        }
    }
    assert(index!=-1);
    if (tile_sizes_update[v] > 1) {
        split_dim(s, index, tile_sizes_update[v],
                  out_up_estimates, "tile");
        move_dim_to_outermost(s, index + 1);
        if (can_parallelize_rvar(v, g_out.name(), u)) {
            int o_dim = s.dims().size() - 2;
            par_vars.insert(s.dims()[o_dim].var);
            par_vars.insert(s.dims()[index].var);
        }
    } else if (tile_sizes_update[v] == 1) {
        move_dim_to_outermost(s, index);
    }
}

// Vectorization of update definitions
if(auto_vec) {
    vectorize_update(g_out, i, out_up_estimates, part.arch_params.vec_len,
                     par_vars);
}

if(auto_par) {
    int curr_par = 1;
    // Exploiting nested parallelism
    for (int i = (int)dims.size() - 2; i > 0 ; i--) {
        bool dim_par = can_parallelize_rvar(dims[i].var,
                                            g_out.name(), u);
        dim_par = dim_par ||
                (par_vars.find(dims[i].var) != par_vars.end());
        if (dim_par) {
            curr_par = curr_par * out_up_estimates[dims[i].var];
            parallelize_dim(s, i);
            move_dim_to_outermost(s, i);
            int outer_dim = dims.size() - 2;
            parallelize_dim(s, outer_dim);
            if (curr_par > parallelism)
                break;
        }
    }
}
}
}

// std::cerr << "Finished updates "  <<  g_out.name() << std::endl;
for (auto &m: part.groups[g_name]) {
    int outer_dim = dims.size() - 2;
    map<string, int> org_mem_estimates =
            get_dim_estimates(m.name(), group_bounds, env);
    map<string, int> mem_estimates = org_mem_estimates;
    if (m.name() != g_out.name() &&
        inlines.find(m.name()) == inlines.end() && num_tile_dims > 0) {
        //int compute_level = inner_tile_dim;
        int compute_level = outer_dim - num_tile_dims +
                num_fused_dims + 1;
        m.schedule().store_level().func = g_out.name();
        //m.schedule().store_level().var = dims[compute_level+1].var;
        m.schedule().store_level().var = dims[compute_level].var;
        m.schedule().compute_level().func = g_out.name();
        m.schedule().compute_level().var = dims[compute_level].var;
        if (auto_vec) {
            if (check_dim_size(m.schedule(), 0, part.arch_params.vec_len, mem_estimates))
                simple_vectorize(m, mem_estimates, 0, part.arch_params.vec_len);
            else if (check_dim_size(m.schedule(), 0, 8, mem_estimates))
                simple_vectorize(m, mem_estimates, 0, 8);
            else if (check_dim_size(m.schedule(), 0, 4, mem_estimates))
                simple_vectorize(m, mem_estimates, 0, 4);
        }
        if (!m.is_pure()) {
            int num_updates = m.updates().size();
            for (int i = 0; i < num_updates; i ++) {
                // Start with fresh bounds estimates for each update
                map<string, int> mem_up_estimates = org_mem_estimates;
                set<string> par_vars;
                vectorize_update(m, i, mem_up_estimates, part.arch_params.vec_len,
                                 par_vars);
            }
        }
    }
}
// std::cerr << "Finished group members "  <<  g_out.name() << std::endl;
*/

return "";
}

string Partitioner::generate_cpu_schedule(const Target& t) {
    for (auto& g: groups)
        generate_group_cpu_schedule(g.second, t);
    return "";
}

void generate_schedules(const vector<Function>& outputs,
                        const Target& target) {
    // Compute an environment
    map<string, Function> env;
    for (Function f : outputs) {
        map<string, Function> more_funcs = find_transitive_calls(f);
        env.insert(more_funcs.begin(), more_funcs.end());
    }

    // Compute a realization order
    vector<string> order = realization_order(outputs, env);

    // Compute the expression costs for each function in the pipeline

    // Dependence analysis to compute all the regions of upstream functions
    // required to compute a region of the function

    FuncValueBounds func_val_bounds = compute_function_value_bounds(order, env);

    bool estimates_avail = check_estimates_on_outputs(outputs);

    // Inform the user that estimates of output sizes were not available on
    // all the outputs of the pipeline.
    user_assert(estimates_avail) << "Please provide estimates for each \
                                 dimension of the pipeline output functions.";

    map<string, vector<string> > update_args;
    set<string> reductions;
    DependenceAnalysis analy(env, func_val_bounds);

    // Show bounds of all the functions in the pipeline given estimates
    // on outputs. Also report functions where the bounds could not be inferred.
    map<string, Box> pipeline_bounds = get_pipeline_bounds(analy, outputs);
    debug(0) << "Pipeline Bounds:" << '\n';
    disp_regions(pipeline_bounds);



    // TODO: Partitioner which is capable of auto scheduling hierarchically
    MachineParams arch_params;
    arch_params.parallelism = 16;
    arch_params.vec_len = 8;
    arch_params.fast_mem_size = 1024;
    arch_params.balance = 10;

    // Initialize the cost model
    CostModel cost_model(env);

    Partitioner part(pipeline_bounds, arch_params, analy,
                     cost_model, outputs, false);

    for (auto& f: env) {
        FindAllCalls find;
        f.second.accept(&find);
        map<string, int64_t> reuse =
                part.evaluate_reuse(f.first, find.calls);
        debug(0) << f.first << '\n';
        for (auto& dir: reuse) {
            debug(0) << dir.first << " " << dir.second << ',';
        }
        debug(0) << '\n';
    }

    part.group(Partitioner::INLINE);

    part.disp_grouping();

    // TODO: Auto scheduler modes
    // O1 Does not introduce any redundant compute but performs basic fusion
    // O2 No redundant compute basic fusion and reordering
    // O3 Trades-offs redundant work for enhancing locality and parallelism

    // TODO: Realize the generated schedule
    // CPU

    // TODO: Update definitions
    // TODO: Boundary conditions

    // TODO: Realize the generated schedule

    // Set the schedule defaults for each function in the environment
    //set_schedule_defaults(env);
    part.generate_cpu_schedule(target);

    // GPU
    // ...
}

}
}
