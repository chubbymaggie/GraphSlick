/*
GraphSlick (c) Elias Bachaalany
-------------------------------------

Plugin module

This module is responsible for handling IDA plugin code

History
--------

10/15/2013 - eliasb             - First version
10/21/2013 - eliasb             - Working chooser / graph renderer
10/22/2013 - eliasb             - Version with working selection of NodeDefLists and GroupDefs
                                - Now the graph-view closes when the panel closes
                                - Factored out functions from the grdata_t class
                                - Wrote initial combine nodes algorithm
10/23/2013 - eliasb             - Polished and completed the combine algorithm
                                - Factored out many code into various modules
10/24/2013 - eliasb             - Added proper coloring on selection (via colorgen class)
                                - Factored out many code into various modules
								- Fixed crash on re-opening the plugin chooser
*/

#pragma warning(disable: 4018 4800)

#include <ida.hpp>
#include <idp.hpp>
#include <graph.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include "groupman.h"
#include "util.h"
#include "algo.hpp"
#include "colorgen.h"

//--------------------------------------------------------------------------
#define MY_TABSTR "    "

//--------------------------------------------------------------------------
static const char STR_CANNOT_BUILD_F_FC[] = "Cannot build function flowchart!";
static const char STR_GS_PANEL[]          = "Graph Slick - Panel";
static const char STR_GS_VIEW[]           = "Graph Slick - View";
static const char STR_OUTWIN_TITLE[]      = "Output window";
static const char STR_IDAVIEWA_TITLE[]    = "IDA View-A";

//--------------------------------------------------------------------------
typedef std::map<int, bgcolor_t> ncolormap_t;

//--------------------------------------------------------------------------
/**
* @brief Graph data/context
*/
struct grdata_t
{
private:
  gnodemap_t node_map;
  ea_t ea;

public:
  int             cur_node;
  graph_viewer_t  *gv;
  ncolormap_t     sel_nodes;
  TForm           *form;
  grdata_t        **parent_ref;
  groupman_t      *gm;

  enum refresh_modes_e
  {
    rfm_soft    = 0,
    rfm_rebuild = 1,
  };

  int refresh_mode;

  /**
  * @brief Constructor
  */
  grdata_t(ea_t ea)
  {
    cur_node = 0;
    gv = NULL;
    this->ea = ea;
    form = NULL;
    refresh_mode = rfm_soft;
    parent_ref = NULL;
  }

  /**
  * @brief Return node data
  */
  gnode_t *get_node(int nid)
  {
    return node_map.get(nid);
  }

  /**
  * @brief Static graph callback
  */
  static int idaapi _gr_callback(
      void *ud, 
      int code, va_list va)
  {
    return ((grdata_t *)ud)->gr_callback(code, va);
  }

  /**
  * @brief 
  */
  int idaapi gr_callback(
    int code,
    va_list va)
  {
    int result = 0;
    switch (code)
    {
      case grcode_changed_current:
      {
        va_arg(va, graph_viewer_t *);
        cur_node = va_argi(va, int);
        //TODO: select all nodes in its group (NDL)
        break;
      }

      // Redraw the graph
      case grcode_user_refresh:
      {
        mutable_graph_t *mg = va_arg(va, mutable_graph_t *);
        if (node_map.empty() || refresh_mode == rfm_rebuild)
        {
          func_to_mgraph(this->ea, mg, node_map);
          //;!;!
          //fc_to_combined_mg_t()(this->ea, gm, node_map, mg);
        }
        result = 1;
        break;
      }

      // retrieve text and background color for the user-defined graph node
      case grcode_user_text:    
      {
        va_arg(va, mutable_graph_t *);
        int node           = va_arg(va, int);
        const char **text  = va_arg(va, const char **);
        bgcolor_t *bgcolor = va_arg(va, bgcolor_t *);

        *text = get_node(node)->text.c_str();
        ncolormap_t::iterator psel = sel_nodes.find(node);
        if (bgcolor != NULL && psel != sel_nodes.end())
          *bgcolor = psel->second;

        result = 1;
        break;
      }
      // The graph is being destroyed
      case grcode_destroyed:
      {
        gv = NULL;
        form = NULL;
        if (parent_ref != NULL)
          *parent_ref = NULL;

        delete this;
        result = 1;
        break;
      }
    }
    return result;
  }
};

//--------------------------------------------------------------------------
static grdata_t *show_graph(
    ea_t ea = BADADDR, 
    groupman_t *gm = NULL)
{
  if (ea == BADADDR)
    ea = get_screen_ea();

  func_t *f = get_func(ea);

  if (f == NULL)
  {
    msg("No function here!\n");
    return NULL;
  }

  // Loop twice: 
  // - (1) Create the graph and exit or close it if it was there 
  // - (2) Re create graph due to last step
  for (int i=0;i<2;i++)
  {
    HWND hwnd = NULL;
    TForm *form = create_tform(STR_GS_VIEW, &hwnd);
    if (hwnd != NULL)
    {
      // get a unique graph id
      netnode id;
      qstring title;
      title.sprnt("$ Combined Graph of %a()", f->startEA);
      id.create(title.c_str());
      grdata_t *ctx = new grdata_t(f->startEA);
      ctx->gm = gm;
      graph_viewer_t *gv = create_graph_viewer(
        form,  
        id, 
        ctx->_gr_callback, 
        ctx, 
        0);
      open_tform(form, FORM_TAB|FORM_MENU|FORM_QWIDGET);
      if (gv != NULL)
      {
        ctx->gv = gv;
        ctx->form = form;
        viewer_fit_window(gv);
      }
      return ctx;
    }
    else
    {
      close_tform(form, 0);
    }
  }
  return NULL;
}

//--------------------------------------------------------------------------
enum chooser_node_type_t
{
  chnt_gm  = 0,
  chnt_gd  = 1,
  chnt_nl  = 3,
};

//--------------------------------------------------------------------------
class chooser_node_t
{
public:
  chooser_node_type_t type;
  groupman_t *gm;
  groupdef_t *gd;
  nodegroup_listp_t *ngl;
  nodedef_list_t *nl;

  chooser_node_t()
  {
    gm = NULL;
    gd = NULL;
    ngl = NULL;
    nl = NULL;
  }
};

typedef qvector<chooser_node_t> chooser_node_vec_t;

//--------------------------------------------------------------------------
/**
* @brief GraphSlick chooser class
*/
class gschooser_t
{
private:
  static gschooser_t *singleton;
  chooser_node_vec_t ch_nodes;

  chooser_info_t chi;
  grdata_t *gr;
  groupman_t *gm;

  static uint32 idaapi s_sizer(void *obj)
  {
    return ((gschooser_t *)obj)->sizer();
  }

  static void idaapi s_getl(void *obj, uint32 n, char *const *arrptr)
  {
    ((gschooser_t *)obj)->getl(n, arrptr);
  }

  static uint32 idaapi s_del(void *obj, uint32 n)
  {
    return ((gschooser_t *)obj)->del(n);
  }

  static void idaapi s_ins(void *obj)
  {
    ((gschooser_t *)obj)->ins();
  }

  static void idaapi s_enter(void *obj, uint32 n)
  {
    ((gschooser_t *)obj)->enter(n);
  }

  static void idaapi s_refresh(void *obj)
  {
    ((gschooser_t *)obj)->refresh();
  }

  static void idaapi s_initializer(void *obj)
  {
    ((gschooser_t *)obj)->initializer();
  }

  static void idaapi s_destroyer(void *obj)
  {
    ((gschooser_t *)obj)->destroyer();
  }

  static void idaapi s_select(void *obj, const intvec_t &sel)
  {
    ((gschooser_t *)obj)->select(sel);
  }

  /**
  * @brief Delete the singleton instance if applicable
  */
  void delete_singleton()
  {
    if ((chi.flags & CH_MODAL) != 0)
      return;

    delete singleton;
    singleton = NULL;
  }

  /**
  * @brief Handles instant node selection in the chooser
  */
  void select(const intvec_t &sel)
  {
    // Delegate this task to the 'enter' routine
    enter(sel[0]);
  }

  /**
  * @brief Return the items count
  */
  uint32 sizer()
  {
    return ch_nodes.size();
  }

  /**
  * @brief Return chooser line description
  */
  void get_node_desc(chooser_node_t *node, qstring *out)
  {
    switch (node->type)
    {
      // Handle a group file node
      case chnt_gm:
      {
        *out = qbasename(node->gm->get_source_file());
        break;
      }
      // Handle group definitions
      case chnt_gd:
      {
        out->sprnt(MY_TABSTR "%s (%s) NGL(%d)", 
          node->gd->groupname.c_str(), 
          node->gd->id.c_str(),
          node->ngl->size());
        break;
      }
      // Handle a node definition list
      case chnt_nl:
      {
        size_t sz = node->nl->size();
        out->sprnt(MY_TABSTR MY_TABSTR "NDL(%d):(", sz);
        for (nodedef_list_t::iterator it=node->nl->begin();
              it != node->nl->end();
              ++it)
        {
          nodedef_t *nd = &*it;
          out->cat_sprnt("%d:%a:%a", nd->nid, nd->start, nd->end);
          if (--sz > 0)
            out->append(", ");
        }
        out->append(")");
        break;
      }
    } // switch
  }

  /**
  * @brief Get textual representation of a given line
  */
  void getl(uint32 n, char *const *arrptr)
  {
    // Return the column name
    if (n == 0)
    {
      qstrncpy(arrptr[0], "Node", MAXSTR);
      return;
    }
    // Return description about a node
    else if (n >= 1)
    {
      --n;
      if (n >= ch_nodes.size())
        return;

      chooser_node_t &cn = ch_nodes[n];
      qstring desc;
      get_node_desc(&cn, &desc);
      qstrncpy(arrptr[0], desc.c_str(), MAXSTR);
    }
  }

  /**
  * @brief 
  * @param 
  * @return
  */
  uint32 del(uint32 n)
  {
    // nop
    return n;
  }

  /**
  * @brief 
  * @param 
  * @return
  */
  void ins()
  {
    //TODO: askfolder()
    //      load the bbgroup file
  }

#define DECL_CG \
  colorgen_t cg; \
  cg.L_INT = -15; \
  colorvargen_t cv; \
  bgcolor_t clr

  /**
  * @brief 
  */
  unsigned int get_color_anyway(colorgen_t &cg, colorvargen_t &cv)
  {
    bgcolor_t clr;

    while (true)
    {
      // Get a color variant
      clr = cv.get_color();
      if (clr != 0)
        break;
      // No variant? Pick a new color
      if (!cg.get_colorvar(cv))
      {
        // No more colors, just rewind
        cg.rewind();
        cg.get_colorvar(cv);
      }
    }
    return clr;
  }

  /**
  * @brief Callback that handles ENTER or double clicks on a chooser node
  */
  void enter(uint32 n)
  {
    if (!IS_SEL(n) || n > ch_nodes.size())
      return;

    chooser_node_t &chn = ch_nodes[n-1];

    switch (chn.type)
    {
      case chnt_gm:
      {
        DECL_CG;
 
        // Walk all groups
        groupdef_listp_t *groups = gm->get_groups();
        for (groupdef_listp_t::iterator it=groups->begin();
             it != groups->end();
             ++it)
        {
          // Get the group definition -> node groups in this def
          groupdef_t *gd = *it;

          // Assign a new color variant for each groupdef
          cg.get_colorvar(cv);
          for (nodegroup_listp_t::iterator it=gd->nodegroups.begin();
               it != gd->nodegroups.end();
               ++it)
          {
            // Use a new color variant for each NDL
            clr = get_color_anyway(cg, cv);
            nodedef_list_t *nl = *it;
            for (nodedef_list_t::iterator it = nl->begin();
              it != nl->end();
              ++it)
            {
              gr->sel_nodes[it->nid] = clr;
            }
          }
        }
        break;
      }
      // Handle double click
      case chnt_nl:
      case chnt_gd:
      {
        if (gr == NULL || gr->gv == NULL)
          break;

        DECL_CG;

        gr->sel_nodes.clear();
        if (chn.type == chnt_nl)
        {
          cg.get_colorvar(cv);
          clr = get_color_anyway(cg, cv);
          for (nodedef_list_t::iterator it = chn.nl->begin();
               it != chn.nl->end();
               ++it)
          {
            gr->sel_nodes[it->nid] = clr;
          }
        }
        // chnt_gd
        else
        {
          // Use one color for all the different group defs
          cg.get_colorvar(cv);
          for (nodegroup_listp_t::iterator it=chn.ngl->begin();
                  it != chn.ngl->end();
                  ++it)
          {
            // Use a new color variant for each NDL
            clr = get_color_anyway(cg, cv);
            nodedef_list_t *nl = *it;
            for (nodedef_list_t::iterator it = nl->begin();
                 it != nl->end();
                 ++it)
            {
              gr->sel_nodes[it->nid] = clr;
            }
          }
        }
        // Soft refresh
        refresh_viewer(gr->gv);
        break;
      }
    }
  }

  /**
  * @brief Close the graph view
  */
  void close_graph()
  {
    if (gr == NULL || gr->form == NULL)
      return;
    close_tform(gr->form, 0);
  }

  /**
  * @brief The chooser is closed
  */
  void destroyer()
  {
    close_graph();
    delete_singleton();
  }

  /**
  * @brief 
  * @param 
  * @return
  */
  void idaapi refresh()
  {
  }

  void idaapi initializer()
  {
    const char *fn;
    fn = "P:\\projects\\experiments\\bbgroup\\sample_c\\bin\\v1\\x86\\f1.bbgroup";
    //fn = "P:\\projects\\experiments\\bbgroup\\sample_c\\bin\\v1\\x86\\main.bbgroup";
    if (!load_file(fn))
      return;

    // Show the graph
    nodedef_listp_t *nodes = gm->get_nodes();
    gr = show_graph((*nodes->begin())->start, gm);
    if (gr != NULL)
    {
      //TODO:
      gr->parent_ref = &gr;
    }
  }

public:
  gschooser_t()
  {
    memset(&chi, 0, sizeof(chi));
    chi.cb = sizeof(chi);
    chi.flags = 0;
    chi.width = -1;
    chi.height = -1;
    chi.title = STR_GS_PANEL;
    chi.obj = this;
    chi.columns = 1;

    static const int widths[] = {60};
    chi.widths = widths;

    chi.icon  = -1;
    chi.deflt = -1;

    chi.sizer       = s_sizer;
    chi.getl        = s_getl;
    chi.ins         = s_ins;
    chi.del         = s_del;
    chi.enter       = s_enter;
    chi.destroyer   = s_destroyer;
    chi.refresh     = s_refresh;
    chi.select      = s_select;
    chi.initializer = s_initializer;

    //chi.popup_names = NULL;   // first 5 menu item names (insert, delete, edit, refresh, copy)
    //static uint32 idaapi *s_update(void *obj, uint32 n);
    //void (idaapi *edit)(void *obj, uint32 n);
    //static int idaapi s_get_icon)(void *obj, uint32 n);
    //void (idaapi *get_attrs)(void *obj, uint32 n, chooser_item_attrs_t *attrs);

    gr = NULL;
    gm = NULL;
  }

  /**
  * @brief Load the file bbgroup file into the chooser
  */
  bool load_file(const char *filename)
  {
    // Load a file and parse it
    delete gm;
    gm = new groupman_t();
    if (!gm->parse(filename))
    {
      msg("error: failed to parse group file '%s'\n", filename);
      delete gm;
      return false;
    }

    // Add the first-level node = bbgroup file
    chooser_node_t *node = &ch_nodes.push_back();
    node->type = chnt_gm;
    node->gm = gm;

    for (groupdef_listp_t::iterator it=gm->get_groups()->begin();
         it != gm->get_groups()->end();
         ++it)
    {
      groupdef_t &gd = **it;

      // Add the second-level node = a set of group defs
      node = &ch_nodes.push_back();
      nodegroup_listp_t &ngl = gd.nodegroups;
      node->type = chnt_gd;
      node->gd   = &gd;
      node->gm   = gm;
      node->ngl  = &ngl;

      // Add each nodedef list within each node group
      for (nodegroup_listp_t::iterator it = ngl.begin();
           it != ngl.end();
           ++it)
      {
        nodedef_list_t *nl = *it;
        // Add the third-level node = nodedef
        node = &ch_nodes.push_back();
        node->type = chnt_nl;
        node->nl = nl;
        node->ngl = &ngl;
        node->gm  = gm;
        node->gd  = &gd;
      }
    }
    return true;
  }

  static void show()
  {
    if (singleton == NULL)
      singleton = new gschooser_t();
    choose3(&singleton->chi);
    set_dock_pos(STR_GS_PANEL, STR_OUTWIN_TITLE, DP_RIGHT);
  }

  ~gschooser_t()
  {
    //NOTE: IDA will close the chooser for us and thus the destroy callback will be called
  }
};
gschooser_t *gschooser_t::singleton = NULL;

//--------------------------------------------------------------------------
//
//      PLUGIN CALLBACKS
//
//--------------------------------------------------------------------------


//--------------------------------------------------------------------------
void idaapi run(int /*arg*/)
{
  gschooser_t::show();
}

//--------------------------------------------------------------------------
int idaapi init(void)
{
  return (callui(ui_get_hwnd).vptr != NULL || is_idaq()) ? PLUGIN_OK : PLUGIN_SKIP;
}

//--------------------------------------------------------------------------
void idaapi term(void)
{
}

//--------------------------------------------------------------------------
//
//      PLUGIN DESCRIPTION BLOCK
//
//--------------------------------------------------------------------------
plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  0,                    // plugin flags
  init,                 // initialize

  term,                 // terminate. this pointer may be NULL.

  run,                  // invoke plugin

  "",                   // long comment about the plugin
                        // it could appear in the status line
                        // or as a hint

  "",                   // multiline help about the plugin

  "GraphSlick",         // the preferred short name of the plugin
  "Ctrl-4"              // the preferred hotkey to run the plugin
};