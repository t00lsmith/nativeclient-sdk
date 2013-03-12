// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "level_layer.h"
#include "app_delegate.h"
#include "game_manager.h"

#include "physics_nodes/CCPhysicsSprite.h"
#include "CCLuaEngine.h"

extern "C" {
#include "lua.h"
#include "tolua++.h"
#include "lualib.h"
#include "lauxlib.h"
#include "tolua_fix.h"
}

// Pixels-to-meters ratio for converting screen coordinates
// to Box2D "meters".
#define PTM_RATIO 32
#define SCREEN_TO_WORLD(n) ((n) / PTM_RATIO)
#define WORLD_TO_SCREEN(n) ((n) * PTM_RATIO)
#define VELOCITY_ITERATIONS 8
#define POS_ITERATIONS 1

#define MAX_SPRITES 100

#define DEFAULT_DENSITY 1.0f
#define DEFAULT_FRICTION 0.2f
#define DEFAULT_RESTITUTION 0.1f

USING_NS_CC_EXT;

LevelLayer* LevelLayer::create(int level_number)
{
  LevelLayer* layer = new LevelLayer(level_number);
  if (!layer)
    return NULL;
  layer->init();
  return layer;
}

bool LevelLayer::init() {
  if (!CCLayerColor::initWithColor(ccc4(0,0x8F,0xD8,0xD8)))
    return false;

  setTouchEnabled(true);

  InitPhysics();

  // Load level from lua file.  For now we simple load level 1.
  LoadLua();

  // Calculate brush size
  CCSpriteBatchNode* brush_batch = (CCSpriteBatchNode*)getChildByTag(TAG_BRUSH);
  assert(brush_batch);
  brush_ = CCSprite::createWithTexture(brush_batch->getTexture());
  brush_->retain();
  CCSize brush_size = brush_->getContentSize();
  brush_radius_ = MAX(brush_size.height/2, brush_size.width/2);

  // Schedule physics updates each frame
  schedule(schedule_selector(LevelLayer::UpdateWorld));
  return true;
}

LevelLayer::LevelLayer(int level_number) :
   level_number_(level_number),
   goal_reached_(false),
   current_touch_id_(-1),
   render_target_(NULL),
#ifdef COCOS2D_DEBUG
   debug_enabled_(false),
#endif
   box2d_density_(DEFAULT_DENSITY),
   box2d_restitution_(DEFAULT_RESTITUTION),
   box2d_friction_(DEFAULT_FRICTION) {
  memset(stars_collected_, 0, sizeof(stars_collected_));
}

LevelLayer::~LevelLayer() {
  brush_->release();
  delete box2d_world_;
#ifdef COCOS2D_DEBUG
  delete box2d_debug_draw_;
#endif
}

void LevelLayer::registerWithTouchDispatcher() {
  CCDirector* director = CCDirector::sharedDirector();
  director->getTouchDispatcher()->addTargetedDelegate(this, 0, true);
}

void LevelLayer::CreateRenderTarget() {
  // create render target for shape drawing
  assert(!render_target_);
  CCSize win_size = CCDirector::sharedDirector()->getWinSize();
  render_target_ = CCRenderTexture::create(win_size.width,
                                           win_size.height,
                                           kCCTexture2DPixelFormat_RGBA8888);
  render_target_->setPosition(ccp(win_size.width / 2, win_size.height / 2));
  addChild(render_target_);
}

bool LevelLayer::LoadLua() {
  CCScriptEngineManager* manager = CCScriptEngineManager::sharedManager();
  CCLuaEngine* engine = (CCLuaEngine*)manager->getScriptEngine();
  assert(engine);
  lua_stack_ = engine->getLuaStack();
  assert(lua_stack_);

  lua_stack_->pushCCObject(this, "LevelLayer");
  lua_stack_->pushInt(level_number_);
  int rtn = lua_stack_->executeFunctionByName("LoadLevel", 2);
  if (rtn == -1) {
    assert(false && "level loading failed");
    return false;
  }

  return true;
}

bool LevelLayer::InitPhysics() {
  b2Vec2 gravity(0.0f, -9.8f);
  box2d_world_ = new b2World(gravity);
  box2d_world_->SetAllowSleeping(true);
  box2d_world_->SetContinuousPhysics(true);

  // Find visible rect, and convert to box2d space.
  CCPoint origin = CCDirector::sharedDirector()->getVisibleOrigin();
  CCSize visible_size = CCDirector::sharedDirector()->getVisibleSize();
  float world_width = SCREEN_TO_WORLD(visible_size.width);
  b2Vec2 world_origin(SCREEN_TO_WORLD(origin.x), SCREEN_TO_WORLD(origin.y));

  // create the ground
  b2BodyDef ground_def;
  ground_def.position = world_origin;
  b2Body* ground_body = box2d_world_->CreateBody(&ground_def);

  CCLog("origin: %.fx%.f", origin.x, origin.y);
  CCLog("size: %.fx%f", visible_size.width, visible_size.height);

  // Define the ground box shape.
  b2EdgeShape ground_box;
  ground_box.Set(b2Vec2(0, 0), b2Vec2(world_width, 0));
  ground_body->CreateFixture(&ground_box, 0);

  box2d_world_->SetContactListener(this);

#ifdef COCOS2D_DEBUG
  box2d_debug_draw_ = new GLESDebugDraw(PTM_RATIO);
  box2d_world_->SetDebugDraw(box2d_debug_draw_);

  uint32 flags = 0;
  flags += b2Draw::e_shapeBit;
  flags += b2Draw::e_jointBit;
  flags += b2Draw::e_centerOfMassBit;
  //flags += b2Draw::e_aabbBit;
  //flags += b2Draw::e_pairBit;
  box2d_debug_draw_->SetFlags(flags);
#endif
  return true;
}

void LevelLayer::ToggleDebug() {
  debug_enabled_ = !debug_enabled_;

  // Set visibility of all children based on debug_enabled_
  CCArray* children = getChildren();
  if (!children)
    return;
  for (uint i = 0; i < children->count(); i++)
  {
    CCNode* child = static_cast<CCNode*>(children->objectAtIndex(i));
    if (child == render_target_)
      continue;
    child->setVisible(!debug_enabled_);
  }
}

CCRect CalcBoundingBox(CCSprite* sprite) {
  CCSize size = sprite->getContentSize();
  CCPoint pos = sprite->getPosition();
  return CCRectMake(pos.x - size.width, pos.y - size.height,
                    size.width, size.height/2);
}

void LevelLayer::UpdateWorld(float dt) {
  // update physics
  box2d_world_->Step(dt, VELOCITY_ITERATIONS, POS_ITERATIONS);
}

void LevelLayer::LuaNotifyContact(b2Contact* contact,
                                    const char* function_name) {
  // Return early if lua didn't define the function_name
  lua_State* state = lua_stack_->getLuaState();
  lua_getglobal(state, function_name);
  bool is_func = lua_isfunction(state, -1);
  lua_pop(state, 1);

  if (!is_func)
    return;

  // Only send to lua collitions between body's that
  // have been tagged.
  b2Body* body1 = contact->GetFixtureA()->GetBody();
  b2Body* body2 = contact->GetFixtureB()->GetBody();
  int tag1 = (int)body1->GetUserData();
  int tag2 = (int)body2->GetUserData();
  if (!tag1 || !tag2)
    return;

  // Call the lua function callback passing in tags of the two bodies that
  // collided
  lua_stack_->pushInt(tag1);
  lua_stack_->pushInt(tag2);
  lua_stack_->executeFunctionByName(function_name, 2);
}

void LevelLayer::BeginContact(b2Contact* contact) {
  LuaNotifyContact(contact, "BeginContact");
}

void LevelLayer::EndContact(b2Contact* contact) {
  LuaNotifyContact(contact, "EndContact");
}

void LevelLayer::LevelComplete() {
  unschedule(schedule_selector(LevelLayer::UpdateWorld));
  setTouchEnabled(false);
  CCScene* scene = static_cast<CCScene*>(getParent());
  GameManager::sharedManager()->GameOver(scene, true);
}

void LevelLayer::DrawPoint(CCPoint& location) {
  ClampBrushLocation(location);
  render_target_->begin();
  brush_->setVisible(true);
  brush_->setPosition(ccp(location.x, location.y));
  brush_->visit();
  brush_->setVisible(false);
  render_target_->end();
  points_being_drawn_.push_back(location);
}

void LevelLayer::draw() {
  CCLayerColor::draw();

#ifdef COCOS2D_DEBUG
  if (debug_enabled_) {
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
    kmGLPushMatrix();
    box2d_world_->DrawDebugData();
    kmGLPopMatrix();
  }
#endif
}

void LevelLayer::ClampBrushLocation(CCPoint& point) {
  CCPoint origin = CCDirector::sharedDirector()->getVisibleOrigin();
  CCSize visible_size = CCDirector::sharedDirector()->getVisibleSize();

  float min_x = origin.x + brush_radius_;
  float min_y = origin.y + brush_radius_;
  if (point.x < min_x) point.x = min_x;
  if (point.y < min_y) point.y = min_y;

  float max_x = origin.x + visible_size.width - brush_radius_;
  float max_y = origin.y + visible_size.height - brush_radius_;
  if (point.x > max_x) point.x = max_x;
  if (point.y > max_y) point.y = max_y;
}

void LevelLayer::DrawLine(CCPoint& start, CCPoint& end) {
  ClampBrushLocation(start);
  ClampBrushLocation(end);

  // calculate distance moved
  float distance = ccpDistance(start, end);

  // draw the brush sprite into render texture at every point between the old
  // and new cursor positions
  render_target_->begin();
  for (int i = 0; i < int(distance + 0.5); i++) {
    float difx = end.x - start.x;
    float dify = end.y - start.y;
    float delta = (float)i / distance;
    brush_->setVisible(true);
    brush_->setPosition(
        ccp(start.x + (difx * delta), start.y + (dify * delta)));

    brush_->visit();
    brush_->setVisible(false);
  }
  render_target_->end();
  points_being_drawn_.push_back(end);
}

bool LevelLayer::ccTouchBegan(CCTouch* touch, CCEvent* event) {
  if (current_touch_id_ != -1)
    return false;

  current_touch_id_ = touch->getID();

  if (!render_target_)
    CreateRenderTarget();

  points_being_drawn_.clear();
  CCPoint location = touch->getLocation();
  DrawPoint(location);
  return true;
}

void LevelLayer::ccTouchMoved(CCTouch* touch, CCEvent* event) {
  assert(touch->getID() == current_touch_id_);
  CCPoint end = touch->getLocation();
  CCPoint start = touch->getPreviousLocation();
  DrawLine(start, end);
}

void LevelLayer::ccTouchEnded(CCTouch* touch, CCEvent* event) {
  assert(touch->getID() == current_touch_id_);
  b2Body* body = CreatePhysicsBody();
  CCSprite* sprite = CreatePhysicsSprite(body);
  addChild(sprite);
  if (debug_enabled_)
    sprite->setVisible(false);

  // release render target (it will get recreated on next touch).
  removeChild(render_target_, true);
  render_target_ = NULL;
  current_touch_id_ = -1;
}

CCRect CalcBodyBounds(b2Body* body) {
  CCSize s = CCDirector::sharedDirector()->getWinSize();

  float minX = FLT_MAX;
  float maxX = 0;
  float minY = FLT_MAX;
  float maxY = 0;

  const b2Transform& xform = body->GetTransform();
  for (b2Fixture* f = body->GetFixtureList(); f; f = f->GetNext()) {
    b2Shape* shape = f->GetShape();
    if (shape->GetType() == b2Shape::e_circle) {
      b2CircleShape* c = static_cast<b2CircleShape*>(shape);
      b2Vec2 center = b2Mul(xform, c->m_p);
      if (center.x - c->m_radius < minX)
        minX = center.x - c->m_radius;
      if (center.x + c->m_radius > maxX)
        maxX = center.x + c->m_radius;
      if (center.y - c->m_radius < minY)
        minY = center.y - c->m_radius;
      if (center.y + c->m_radius > maxY)
        maxY = center.y + c->m_radius;
    } else {
      b2PolygonShape* poly = static_cast<b2PolygonShape*>(shape);
      int32 vertex_count = poly->m_vertexCount;

      for (int i = 0; i < vertex_count; ++i)
      {
        b2Vec2 vertex = b2Mul(xform, poly->m_vertices[i]);
        if (vertex.x < minX)
          minX = vertex.x;
        if (vertex.x > maxX)
          maxX = vertex.x;
        if (vertex.y < minY)
          minY = vertex.y;
        if (vertex.y > maxY)
          maxY = vertex.y;
      }
    }
  }

  maxX = WORLD_TO_SCREEN(maxX);
  minX = WORLD_TO_SCREEN(minX);
  maxY = WORLD_TO_SCREEN(maxY);
  minY = WORLD_TO_SCREEN(minY);

  float width = maxX - minX;
  float height = maxY - minY;
  float remY = s.height - maxY;
  return CCRectMake(minX, remY, width, height);
}

CCSprite* LevelLayer::CreatePhysicsSprite(b2Body* body) {
  CCPhysicsSprite *sprite;

  // create a new texture based on the current contents of the
  // render target
  CCImage* image = render_target_->newCCImage();
  CCTexture2D* tex = new CCTexture2D();
  tex->initWithImage(image);
  tex->autorelease();
  delete image;

  // Find the bounds of the physics body wihh the target texture
  CCRect sprite_rect = CalcBodyBounds(body);
  sprite_rect.origin.x -= brush_radius_;
  sprite_rect.origin.y -= brush_radius_;
  sprite_rect.size.width += brush_radius_;
  sprite_rect.size.height += brush_radius_;

  CCSize s = CCDirector::sharedDirector()->getWinSize();
  CCPoint body_pos = ccp(WORLD_TO_SCREEN(body->GetPosition().x),
                         WORLD_TO_SCREEN(body->GetPosition().y));


  // Create a new sprite based on the texture
  sprite = CCPhysicsSprite::createWithTexture(tex, sprite_rect);
  sprite->setB2Body(body);
  sprite->setPTMRatio(PTM_RATIO);

  // Set the anchor point of the sprite
  float anchorX = body_pos.x - sprite_rect.origin.x;
  float anchorY = body_pos.y + sprite_rect.origin.y + sprite_rect.size.height;
  anchorY -= s.height;

  // anchor point goes from 0.0 to 1.0 with in bounds of the sprite itself.
  sprite->setAnchorPoint(ccp(anchorX / sprite_rect.size.width,
                             anchorY / sprite_rect.size.height));
  return sprite;
}

b2Body* LevelLayer::CreatePhysicsBody() {
  assert(points_being_drawn_.size());
  CCPoint start_point = points_being_drawn_.front();

  assert(points_being_drawn_.size());
  CCLog("new body from %d points", points_being_drawn_.size());
  // create initial body
  b2BodyDef def;
  def.type = b2_dynamicBody;
  def.position.Set(SCREEN_TO_WORLD(start_point.x),
                   SCREEN_TO_WORLD(start_point.y));
  b2Body* body = box2d_world_->CreateBody(&def);

  const int min_box_length = brush_radius_;

  // Create an initial box the size of the brush
  AddSphereToBody(body, &start_point);
  AddSphereToBody(body, &points_being_drawn_.back());

  // Add boxes to body for every point that was drawn by the
  // user.
  // initialise endpoint to be the second item in the list
  // and iterate until it points to the final element.
  PointList::iterator iter = points_being_drawn_.begin();
  ++iter;
  for (; iter != points_being_drawn_.end(); iter++) {
    CCPoint end_point = *iter;
    float distance = ccpDistance(start_point, end_point);
    // if the distance between points it too small then
    // skip the current point
    if (distance < min_box_length) {
      if (iter != points_being_drawn_.end() - 1)
        continue;
    }
    AddLineToBody(body, start_point, end_point);
    start_point = *iter;
  }

  points_being_drawn_.clear();
  return body;
}

void LevelLayer::AddShapeToBody(b2Body *body, b2Shape* shape) {
  b2FixtureDef shape_def;
  shape_def.shape = shape;
  shape_def.density = box2d_density_;
  shape_def.friction = box2d_friction_;
  shape_def.restitution = box2d_restitution_;
  body->CreateFixture(&shape_def);
}

void LevelLayer::AddSphereToBody(b2Body *body, CCPoint* location) {
  b2CircleShape shape;
  shape.m_radius = SCREEN_TO_WORLD(brush_radius_);
  shape.m_p.x = SCREEN_TO_WORLD(location->x) - body->GetPosition().x;
  shape.m_p.y = SCREEN_TO_WORLD(location->y) - body->GetPosition().y;
  AddShapeToBody(body, &shape);
}

void LevelLayer::AddLineToBody(b2Body *body, CCPoint start, CCPoint end) {
  float distance = ccpDistance(start, end);

  float sx = start.x;
  float sy = start.y;
  float ex = end.x;
  float ey = end.y;
  float dist_x = sx - ex;
  float dist_y = sy - ey;
  float angle = atan2(dist_y, dist_x);

  float posx = SCREEN_TO_WORLD((sx+ex)/2) - body->GetPosition().x;
  float posy = SCREEN_TO_WORLD((sy+ey)/2) - body->GetPosition().y;

  float width = SCREEN_TO_WORLD(abs(distance));
  float height = SCREEN_TO_WORLD(brush_->boundingBox().size.height);

  b2PolygonShape shape;
  shape.SetAsBox(width / 2, height / 2, b2Vec2(posx, posy), angle);
  AddShapeToBody(body, &shape);
}
